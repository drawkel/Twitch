/**
 * @file Messaging.cpp
 *
 * This module contains the implementation of the
 * Twitch::Messaging class.
 *
 * © 2016-2018 by Richard Walters
 */

#include "Message.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <inttypes.h>
#include <list>
#include <map>
#include <mutex>
#include <queue>
#include <regex>
#include <set>
#include <stdint.h>
#include <stdlib.h>
#include <string>
#include <StringExtensions/StringExtensions.hpp>
#include <thread>
#include <Twitch/Messaging.hpp>
#include <vector>

namespace {

    /**
     * This is the required line terminator for lines of text
     * sent to or from Twitch chat servers.
     */
    const std::string CRLF = "\r\n";

    /**
     * This is the maximum amount of time to wait for the Twitch server to
     * provide the Message Of The Day (MOTD), confirming a successful log-in,
     * before timing out.
     */
    constexpr double LOG_IN_TIMEOUT_SECONDS = 5.0;

    /**
     * This regular expression should only match the nickname of an anonymous
     * Twitch user.
     */
    static const std::regex ANONYMOUS_NICKNAME_PATTERN("justinfan([0-9]+)");

    /**
     * This is used to convey an action for the Messaging class worker
     * to either perform or await, including any necessary context.
     */
    struct Action {
        // Types

        /**
         * These are the types of actions which the Messaging class worker can
         * perform.
         */
        enum class Type {
            /**
             * Establish a new connection to Twitch chat, and use the new
             * connection to log in.
             */
            LogIn,

            /**
             * Request the IRCv3 capabilities of the server.
             */
            RequestCaps,

            /**
             * Wait for the message of the day (MOTD) from the server.
             */
            AwaitMotd,

            /**
             * Log out of Twitch chat, and close the active connection.
             */
            LogOut,

            /**
             * Process all messages received from the Twitch server.
             */
            ProcessMessagesReceived,

            /**
             * Handle when the server closes its end of the connection.
             */
            ServerDisconnected,

            /**
             * Join a chat room.
             */
            Join,

            /**
             * Leave a chat room.
             */
            Leave,

            /**
             * Send a message to a channel.
             */
            SendMessage,

            /**
             * Send a whisper to a channel.
             */
            SendWhisper,
        };

        // Properties

        /**
         * This is the type of action to perform.
         */
        Type type;

        /**
         * This is used with multiple actions, to provide the client
         * with the primary nickname associated with the command.
         */
        std::string nickname;

        /**
         * This is used with the LogIn action, to provide the client
         * with the OAuth token to be used to authenticate with the server.
         */
        std::string token;

        /**
         * This is used with multiple actions, to provide the client
         * with some context or text to be sent to the server.
         */
        std::string message;

        /**
         * This is used with the `SendMessage` action.  If not empty,
         * it indicates the `id` of the message to which the message
         * to be sent is intended to reply.
         */
        std::string parent;

        /**
         * This flag is used when the action may be done anonymously or not,
         * to indicate whether or not to be anonymous.
         */
        bool anonymous = false;

        /**
         * This is the time, according to the time keeper, at which
         * the action will be considered timed out.
         */
        double expiration = 0.0;
    };

    /**
     * This function replaces all escape sequences in the given string with
     * their replacements.
     *
     * @note
     *     This really needs to be made generic and moved to the
     *     SystemAbstractions StringExtensions module.
     *
     * @param[in] s
     *     This is the string which may have escape sequences to replace.
     *
     * @return
     *     The given string, with any escaped sequences replaced, is returned.
     */
    std::string UnescapeMessage(const std::string& s) {
        std::string output;
        bool escape = false;
        for (size_t i = 0; i < s.length(); ++i) {
            if (escape) {
                if (s[i] == 's') {
                    output += ' ';
                } else if (s[i] == 'n') {
                    output += '\n';
                } else if (s[i] == ':') {
                    output += ';';
                } else if (s[i] == '\\') {
                    output += '\\';
                }
                escape = false;
            } else if (s[i] == '\\') {
                escape = true;
            } else {
                output += s[i];
            }
        }
        return output;
    }

    /**
     * This method returns the nickname portion of a message prefix.
     *
     * @param[in] prefix
     *     This is the message prefix from which to extract the nickname.
     *
     * @return
     *     The nickname portion of the prefix is returned.
     */
    std::string ExtractNicknameFromPrefix(const std::string& prefix) {
        const auto nicknameDelimiter = prefix.find('!');
        if (nicknameDelimiter == std::string::npos) {
            return "";
        }
        return prefix.substr(0, nicknameDelimiter);
    }

}

namespace Twitch {

    /**
     * This contains the private properties of a Messaging instance.
     */
    struct Messaging::Impl {
        // Types

        /**
         * This is the type of member function pointer used to map
         * action types to their performer methods.
         *
         * @param[in] action
         *     This is the action to perform.
         */
        typedef void(Impl::*ActionPerformer)(Action&& action);

        /**
         * This is the type of member function pointer used to map
         * action types to their timeout methods.
         *
         * @param[in] action
         *     This is the action to time out.
         */
        typedef void(Impl::*ActionTimeout)(Action&& action);

        /**
         * This is the type of member function pointer used to map
         * server commands to their handlers.
         *
         * @param[in] message
         *     This holds information about the server command to handle.
         */
        typedef void(Impl::*ServerCommandHandler)(Message&& message);

        /**
         * This is the type of member function pointer used to map
         * action types to their message processors.
         *
         * @param[in,out] action
         *     This is the action for which to process the given message.
         *
         * @param[in] message
         *     This holds information about the message to process
         *     within the context of the given action.
         *
         * @return
         *     An indication of whether or not the action was completed by
         *     processing the given message is returned.
         */
        typedef bool(Impl::*ActionProcessor)(
            Action& action,
            const Message& message
        );

        /**
         * This is the type of map used to map action types to their message
         * processors.
         */
        typedef std::map< Action::Type, ActionProcessor > ActionProcessors;

        // Properties

        /**
         * This is used to synchronize access to the object.
         */
        std::mutex mutex;

        // --------------------------------------------------------------------
        // All properties in this section are protected by the mutex.
        // ⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇
        // --------------------------------------------------------------------

        /**
         * This is a helper object used to generate and publish
         * diagnostic messages.
         */
        SystemAbstractions::DiagnosticsSender diagnosticsSender;

        /**
         * This is the function to call in order to make a new
         * connection to the Twitch server.
         */
        ConnectionFactory connectionFactory;

        /**
         * This is the object to use to measure elapsed time periods.
         */
        std::shared_ptr< TimeKeeper > timeKeeper;

        /**
         * This is the object provided by the user of this class, in order to
         * receive notifications, events, and other callbacks from the class.
         */
        std::shared_ptr< User > user = std::make_shared< User >();

        /**
         * This is used to signal the worker thread to wake up.
         */
        std::condition_variable wakeWorker;

        /**
         * This flag indicates whether or not the worker thread
         * should be stopped.
         */
        bool stopWorker = false;

        /**
         * These are the actions to be performed by the worker thread.
         */
        std::deque< Action > actionsToBePerformed;

        /**
         * This is used to perform background tasks for the object.
         */
        std::thread worker;

        // --------------------------------------------------------------------
        // ⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆
        // All properties in this section are protected by the mutex.
        // --------------------------------------------------------------------

        // --------------------------------------------------------------------
        // All properties in this section should only be used by the worker
        // thread.
        // ⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇⬇
        // --------------------------------------------------------------------

        /**
         * This is the interface to the current connection to the Twitch
         * server, if we are connected.
         */
        std::shared_ptr< Connection > connection;

        /**
         * This is essentially just a buffer to receive raw characters from the
         * Twitch server, until a complete line has been received, removed from
         * this buffer, and handled appropriately.
         */
        std::string dataReceived;

        /**
         * If true, this flag indicates that the user is not going to be
         * offering an OAuth token to authenticate as a registered user/bot,
         * and will only be able to receive messages, not send them.
         */
        bool anonymous = false;

        /**
         * This flag indicates whether or not the client has finished
         * logging into the Twitch server (we've received the Message Of
         * The Day (MOTD) from the server).
         */
        bool loggedIn = false;

        /**
         * This holds onto any actions for which the worker is awaiting a
         * response from the server.
         */
        std::list< Action > actionsAwaitingResponses;

        /**
         * These are the IRCv3 capabilities advertised by the server.
         */
        std::set< std::string > capsSupported;

        // --------------------------------------------------------------------
        // ⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆⬆
        // All properties in this section should only be used by the worker
        // thread.
        // --------------------------------------------------------------------

        // Methods

        /**
         * This is the constructor for the structure.
         */
        Impl()
            : diagnosticsSender("TMI")
        {
        }

        /**
         * This method is called to send a raw line of text
         * to the Twitch server.  Do not include the line terminator (CRLF),
         * as this method adds one to the end when sending the line.
         *
         * @param[in,out] connection
         *     This is the connection to use to send the message.
         *
         * @param[in] rawLine
         *     This is the raw line of text to send to the Twitch server.
         *     Do not include the line terminator (CRLF),
         *     as this method adds one to the end when sending the line.
         */
        void SendLineToTwitchServer(
            Connection& connection,
            const std::string& rawLine
        ) {
            if (rawLine.substr(0, 11) == "PASS oauth:") {
                diagnosticsSender.SendDiagnosticInformationString(0, "< PASS oauth:**********************");
            } else {
                diagnosticsSender.SendDiagnosticInformationString(0, "< " + rawLine);
            }
            connection.Send(rawLine + CRLF);
        }

        /**
         * This method is called whenever any message is received from the
         * Twitch server for the user agent.
         *
         * @param[in] rawText
         *     This is the raw text received from the Twitch server.
         */
        void OnMessageReceived(const std::string& rawText) {
            std::lock_guard< decltype(mutex) > lock(mutex);
            Action action;
            action.type = Action::Type::ProcessMessagesReceived;
            action.message = rawText;
            actionsToBePerformed.push_back(action);
            wakeWorker.notify_one();
        }

        /**
         * This method is called when the Twitch server closes its end of the
         * connection.
         */
        void OnServerDisconnected() {
            std::lock_guard< decltype(mutex) > lock(mutex);
            Action action;
            action.type = Action::Type::ServerDisconnected;
            actionsToBePerformed.push_back(action);
            wakeWorker.notify_one();
        }

        /**
         * This method is called to request additional IRC capabilities for the
         * connection with the Twitch chat server.
         *
         * @param[in] action
         *     This holds the information needed to log into Twitch chat.
         */
        void RequestCapabilities(Action action) {
            SendLineToTwitchServer(*connection, "CAP REQ :twitch.tv/commands twitch.tv/membership twitch.tv/tags");
            action.type = Action::Type::RequestCaps;
            if (timeKeeper != nullptr) {
                action.expiration = timeKeeper->GetCurrentTime() + LOG_IN_TIMEOUT_SECONDS;
            }
            actionsAwaitingResponses.push_back(std::move(action));
        }

        /**
         * This method is called to finish the capabilities negotiation phase
         * of logging into Twitch chat, send the user's authentication
         * information, and begin waiting for the Message of the Day (MOTD)
         * from the server, to indicate that we're logged in successfully.
         *
         * @param[in] action
         *     This holds the information needed to log into Twitch chat.
         */
        void EndCapabilitiesHandshakeAndAuthenticate(Action action) {
            SendLineToTwitchServer(*connection, "CAP END");
            if (!anonymous) {
                SendLineToTwitchServer(*connection, "PASS oauth:" + action.token);
            }
            SendLineToTwitchServer(*connection, "NICK " + action.nickname);
            action.type = Action::Type::AwaitMotd;
            if (timeKeeper != nullptr) {
                action.expiration = timeKeeper->GetCurrentTime() + LOG_IN_TIMEOUT_SECONDS;
            }
            actionsAwaitingResponses.push_back(std::move(action));
        }

        /**
         * This method is called whenever the user agent disconnects from the
         * Twitch server.
         *
         * @param[in] farewell
         *     If not empty, the user agent should sent a QUIT command before
         *     disconnecting, and this is a message to include in the
         *     QUIT command.
         */
        void Disconnect(const std::string farewell = "") {
            if (connection == nullptr) {
                return;
            }
            if (!farewell.empty()) {
                connection->Send("QUIT :" + farewell + CRLF);
            }
            connection->Disconnect();
            user->LogOut();
            connection = nullptr;
            loggedIn = false;
            actionsAwaitingResponses.clear();
            capsSupported.clear();
        }

        /**
         * This method is called to process the given message through all
         * actions awaiting responses, removing any actions that are completed
         * after processing the message.
         *
         * @param[in] message
         *     This is the message to process through all actions awaiting
         *     responses.
         *
         * @param[in] actionProcessors
         *     This is the map of action types to processing methods to use
         *     for the given message.
         */
        void ProcessMessageWithAwaitingActions(
            const Message& message,
            const ActionProcessors& actionProcessors
        ) {
            for (
                auto it = actionsAwaitingResponses.begin(),
                end = actionsAwaitingResponses.end();
                it != end;
            ) {
                auto& action = *it;
                const auto actionProcessor = actionProcessors.find(action.type);
                if (
                    (actionProcessor != actionProcessors.end())
                    && (this->*(actionProcessor->second))(action, message)
                ) {
                    it = actionsAwaitingResponses.erase(it);
                } else {
                    ++it;
                }
            }
        }

        /**
         * This method is called from the worker thread to time out any actions
         * that are awaiting responses but have expired.
         */
        void ProcessTimeouts() {
            const auto now = timeKeeper->GetCurrentTime();
            std::queue< Action > actionsToTimeOut;
            for (
                auto it = actionsAwaitingResponses.begin(),
                end = actionsAwaitingResponses.end();
                it != end;
            ) {
                auto& action = *it;
                if (now >= action.expiration) {
                    actionsToTimeOut.push(std::move(action));
                    it = actionsAwaitingResponses.erase(it);
                } else {
                    ++it;
                }
            }
            while (!actionsToTimeOut.empty()) {
                auto& action = actionsToTimeOut.front();
                TimeoutAction(std::move(action));
                actionsToTimeOut.pop();
            }
        }

        /**
         * This method posts the given action to the queue of actions to be
         * performed by the worker thread, and wakes the worker thread up.
         *
         * @param[in] action
         *     This is the action to perform.
         */
        void PostAction(Action&& action) {
            std::lock_guard< decltype(mutex) > lock(mutex);
            actionsToBePerformed.push_back(std::move(action));
            wakeWorker.notify_one();
        }

        /**
         * This method performs the given action.
         *
         * @param[in] action
         *     This is the action to perform.
         */
        void PerformAction(Action&& action) {
            static const std::map< Action::Type, ActionPerformer > actionPerformers = {
                {Action::Type::LogIn, &Impl::PerformActionLogIn},
                {Action::Type::LogOut, &Impl::PerformActionLogOut},
                {Action::Type::ProcessMessagesReceived, &Impl::PerformActionProcessMessagesReceived},
                {Action::Type::ServerDisconnected, &Impl::PerformActionServerDisconnected},
                {Action::Type::Join, &Impl::PerformActionJoin},
                {Action::Type::Leave, &Impl::PerformActionLeave},
                {Action::Type::SendMessage, &Impl::PerformActionSendMessage},
                {Action::Type::SendWhisper, &Impl::PerformActionSendWhisper},
            };
            const auto actionPerformer = actionPerformers.find(action.type);
            if (actionPerformer != actionPerformers.end()) {
                (this->*(actionPerformer->second))(std::move(action));
            }
        }

        /**
         * This method times out the given action.
         *
         * @param[in] action
         *     This is the action to time out.
         */
        void TimeoutAction(Action&& action) {
            static const std::map< Action::Type, ActionTimeout > actionTimeouts = {
                {Action::Type::LogIn, &Impl::TimeoutActionLogIn},
                {Action::Type::RequestCaps, &Impl::TimeoutActionRequestCaps},
                {Action::Type::AwaitMotd, &Impl::TimeoutActionAwaitMotd},
            };
            const auto actionTimeout = actionTimeouts.find(action.type);
            if (actionTimeout != actionTimeouts.end()) {
                (this->*(actionTimeout->second))(std::move(action));
            }
        }

        /**
         * This method is used as an action processor when the action should
         * just be discarded without actually doing anything more.
         *
         * @param[in,out] action
         *     This is the action for which to process the given message.
         *
         * @param[in] message
         *     This holds information about the message to process
         *     within the context of the given action.
         *
         * @return
         *     An indication of whether or not the action was completed by
         *     processing the given message is returned.
         */
        bool DiscardAction(
            Action& action,
            const Message& message
        ) {
            return false;
        }

        /**
         * This method performs the given LogIn action.
         *
         * @param[in] action
         *     This is the action to perform.
         */
        void PerformActionLogIn(Action&& action) {
            if (connection != nullptr) {
                return;
            }
            connection = connectionFactory();
            connection->SetMessageReceivedDelegate(
                std::bind(&Impl::OnMessageReceived, this, std::placeholders::_1)
            );
            connection->SetDisconnectedDelegate(
                std::bind(&Impl::OnServerDisconnected, this)
            );
            if (connection->Connect()) {
                capsSupported.clear();
                anonymous = action.anonymous;
                SendLineToTwitchServer(*connection, "CAP LS 302");
                if (timeKeeper != nullptr) {
                    action.expiration = timeKeeper->GetCurrentTime() + LOG_IN_TIMEOUT_SECONDS;
                }
                actionsAwaitingResponses.push_back(std::move(action));
            } else {
                user->LogOut();
            }
        }

        /**
         * This method processes the given CAP message in context of the given
         * LogIn action.
         *
         * @param[in,out] action
         *     This is the action for which to process the given message.
         *
         * @param[in] message
         *     This holds information about the message to process
         *     within the context of the given action.
         *
         * @return
         *     An indication of whether or not the action was completed by
         *     processing the given message is returned.
         */
        bool ProcessActionLogInCap(
            Action& action,
            const Message& message
        ) {
            if (
                (message.parameters.size() < 3)
                || (message.parameters[1] != "LS")
            ) {
                return false;
            }
            if (message.parameters[2] == "*") {
                const auto newCapsSupported = StringExtensions::Split(message.parameters[3], ' ');
                capsSupported.insert(newCapsSupported.begin(), newCapsSupported.end());
                return false;
            } else {
                const auto newCapsSupported = StringExtensions::Split(message.parameters[2], ' ');
                capsSupported.insert(newCapsSupported.begin(), newCapsSupported.end());
                if (
                    (capsSupported.find("twitch.tv/commands") == capsSupported.end())
                    || (capsSupported.find("twitch.tv/membership") == capsSupported.end())
                    || (capsSupported.find("twitch.tv/tags") == capsSupported.end())
                ) {
                    EndCapabilitiesHandshakeAndAuthenticate(action);
                } else {
                    RequestCapabilities(action);
                }
                return true;
            }
        }

        /**
         * This method times out the given LogIn action.
         *
         * @param[in] action
         *     This is the action to time out.
         */
        void TimeoutActionLogIn(Action&& action) {
            Disconnect("Timeout waiting for capability list");
        }

        /**
         * This method processes the given CAP message in context of the given
         * RequestCaps action.
         *
         * @param[in,out] action
         *     This is the action for which to process the given message.
         *
         * @param[in] message
         *     This holds information about the message to process
         *     within the context of the given action.
         *
         * @return
         *     An indication of whether or not the action was completed by
         *     processing the given message is returned.
         */
        bool ProcessActionRequestCapsCap(
            Action& action,
            const Message& message
        ) {
            if (
                (message.parameters.size() < 2)
                || (
                    (message.parameters[1] != "ACK")
                    && (message.parameters[1] != "NAK")
                )
            ) {
                return false;
            }
            EndCapabilitiesHandshakeAndAuthenticate(action);
            return true;
        }

        /**
         * This method times out the given RequestCaps action.
         *
         * @param[in] action
         *     This is the action to time out.
         */
        void TimeoutActionRequestCaps(Action&& action) {
            Disconnect("Timeout waiting for response to capability request");
        }

        /**
         * This method processes the given Message of the Day (MOTD) message in
         * context of the given AwaitMotd action.
         *
         * @param[in,out] action
         *     This is the action for which to process the given message.
         *
         * @param[in] message
         *     This holds information about the message to process
         *     within the context of the given action.
         *
         * @return
         *     An indication of whether or not the action was completed by
         *     processing the given message is returned.
         */
        bool ProcessActionAwaitMotdMotd(
            Action& action,
            const Message& message
        ) {
            if (!loggedIn) {
                loggedIn = true;
                user->LogIn();
            }
            return true;
        }

        /**
         * This method times out the given AwaitMotd action.
         *
         * @param[in] action
         *     This is the action to time out.
         */
        void TimeoutActionAwaitMotd(Action&& action) {
            Disconnect("Timeout waiting for MOTD");
        }

        /**
         * This method performs the given LogOut action.
         *
         * @param[in] action
         *     This is the action to perform.
         */
        void PerformActionLogOut(Action&& action) {
            Disconnect(action.message);
        }

        /**
         * This method performs the given ProcessMessagesReceived action.
         *
         * @param[in] action
         *     This is the action to perform.
         */
        void PerformActionProcessMessagesReceived(Action&& action) {
            static const std::map< std::string, ServerCommandHandler > serverCommandHandlers = {
                {"353", &Impl::HandleServerCommandNameList},
                {"376", &Impl::HandleServerCommandMotd},
                {"PING", &Impl::HandleServerCommandPing},
                {"JOIN", &Impl::HandleServerCommandJoin},
                {"PART", &Impl::HandleServerCommandPart},
                {"PRIVMSG", &Impl::HandleServerCommandPrivMsg},
                {"CAP", &Impl::HandleServerCommandCap},
                {"WHISPER", &Impl::HandleServerCommandWhisper},
                {"NOTICE", &Impl::HandleServerCommandNotice},
                {"HOSTTARGET", &Impl::HandleServerCommandHostTarget},
                {"ROOMSTATE", &Impl::HandleServerCommandRoomState},
                {"CLEARCHAT", &Impl::HandleServerCommandClearChat},
                {"CLEARMSG", &Impl::HandleServerCommandClearMessage},
                {"MODE", &Impl::HandleServerCommandMode},
                {"GLOBALUSERSTATE", &Impl::HandleServerCommandGlobalUserState},
                {"USERSTATE", &Impl::HandleServerCommandUserState},
                {"RECONNECT", &Impl::HandleServerCommandReconnect},
                {"USERNOTICE", &Impl::HandleServerCommandUserNotice},
            };
            dataReceived += action.message;
            Message message;
            while (Message::Parse(dataReceived, message, diagnosticsSender)) {
                const auto commandHandler = serverCommandHandlers.find(message.command);
                if (commandHandler != serverCommandHandlers.end()) {
                    (this->*(commandHandler->second))(std::move(message));
                }
            }
        }

        /**
         * This method is called to handle the end-of-MOTD command (376) from
         * the Twitch server.
         *
         * @param[in] message
         *     This holds information about the server command to handle.
         */
        void HandleServerCommandMotd(Message&& message) {
            static const ActionProcessors motdActionProcessors = {
                {Action::Type::AwaitMotd, &Impl::ProcessActionAwaitMotdMotd},
            };
            ProcessMessageWithAwaitingActions(
                message,
                motdActionProcessors
            );
        }

        /**
         * This method is called to handle the name list command (353) from
         * the Twitch server.
         *
         * @param[in] message
         *     This holds information about the server command to handle.
         */
        void HandleServerCommandNameList(Message&& message) {
            if (message.parameters.size() != 4) {
                return;
            }
            NameListInfo nameListInfo;
            nameListInfo.channel = message.parameters[2].substr(1);
            nameListInfo.names = StringExtensions::Split(message.parameters[3], ' ');
            user->NameList(std::move(nameListInfo));
        }

        /**
         * This method is called to handle the PING command from the Twitch
         * server.
         *
         * @param[in] message
         *     This holds information about the server command to handle.
         */
        void HandleServerCommandPing(Message&& message) {
            if (message.parameters.size() < 1) {
                return;
            }
            const auto server = message.parameters[0];
            if (connection != nullptr) {
                SendLineToTwitchServer(*connection, "PONG :" + server);
            }
        }

        /**
         * This method is called to handle the JOIN command from the Twitch
         * server.
         *
         * @param[in] message
         *     This holds information about the server command to handle.
         */
        void HandleServerCommandJoin(Message&& message) {
            if (
                (message.parameters.size() < 1)
                || (message.parameters[0].length() < 2)
            ) {
                return;
            }
            const auto nicknameDelimiter = message.prefix.find('!');
            if (nicknameDelimiter == std::string::npos) {
                return;
            }
            const auto nickname = message.prefix.substr(0, nicknameDelimiter);
            if (std::regex_match(nickname, ANONYMOUS_NICKNAME_PATTERN)) {
                return;
            }
            MembershipInfo membershipInfo;
            membershipInfo.user = nickname;
            membershipInfo.channel = message.parameters[0].substr(1);
            user->Join(std::move(membershipInfo));
        }

        /**
         * This method is called to handle the PART command from the Twitch
         * server.
         *
         * @param[in] message
         *     This holds information about the server command to handle.
         */
        void HandleServerCommandPart(Message&& message) {
            if (
                (message.parameters.size() < 1)
                || (message.parameters[0].length() < 2)
            ) {
                return;
            }
            const auto nicknameDelimiter = message.prefix.find('!');
            if (nicknameDelimiter == std::string::npos) {
                return;
            }
            const auto nickname = message.prefix.substr(0, nicknameDelimiter);
            if (std::regex_match(nickname, ANONYMOUS_NICKNAME_PATTERN)) {
                return;
            }
            MembershipInfo membershipInfo;
            membershipInfo.user = nickname;
            membershipInfo.channel = message.parameters[0].substr(1);
            user->Leave(std::move(membershipInfo));
        }

        /**
         * This method is called to handle the PRIVMSG command from the Twitch
         * server.
         *
         * @param[in] message
         *     This holds information about the server command to handle.
         */
        void HandleServerCommandPrivMsg(Message&& message) {
            // Ignore message unless it at least has a channel/user name and
            // message.
            if (message.parameters.size() < 2) {
                return;
            }

            // Extract user name from message prefix.
            MessageInfo messageInfo;
            const auto nickname = ExtractNicknameFromPrefix(message.prefix);
            messageInfo.user = nickname;

            // Copy message content.
            // Check to see if it's an action.
            if (
                (message.parameters[1].length() >= 8)
                && (message.parameters[1][0] == '\x1')
                && (message.parameters[1].substr(1, 6) == "ACTION")
                && (message.parameters[1][message.parameters[1].length() - 1] == '\x1')
            ) {
                messageInfo.isAction = true;
                messageInfo.messageContent = message.parameters[1].substr(7, message.parameters[1].length() - 8);
            } else {
                messageInfo.isAction = false;
                messageInfo.messageContent = message.parameters[1];
            }

            // Parse message ID.
            const auto messageIdTag = message.tags.allTags.find("id");
            if (messageIdTag != message.tags.allTags.end()) {
                messageInfo.messageId = messageIdTag->second;
            }

            // Parse bits.
            const auto bitsTag = message.tags.allTags.find("bits");
            if (bitsTag != message.tags.allTags.end()) {
                if (sscanf(bitsTag->second.c_str(), "%zu", &messageInfo.bits) != 1) {
                    messageInfo.bits = 0;
                }
            }

            // Copy tags.
            messageInfo.tags = message.tags;

            // Trigger callback; if parameter begins with '#', this is a
            // message sent to the channel; otherwise, it's a private message
            // to the user.
            if (message.parameters[0][0] == '#') {
                messageInfo.channel = message.parameters[0].substr(1);
                user->Message(std::move(messageInfo));
            } else {
                user->PrivateMessage(std::move(messageInfo));
            }
        }

        /**
         * This method is called to handle the CAP command from the Twitch
         * server.
         *
         * @param[in] message
         *     This holds information about the server command to handle.
         */
        void HandleServerCommandCap(Message&& message) {
            static const ActionProcessors capActionProcessors = {
                {Action::Type::LogIn, &Impl::ProcessActionLogInCap},
                {Action::Type::RequestCaps, &Impl::ProcessActionRequestCapsCap},
            };
            ProcessMessageWithAwaitingActions(
                message,
                capActionProcessors
            );
        }

        /**
         * This method is called to handle the WHISPER command from the Twitch
         * server.
         *
         * @param[in] message
         *     This holds information about the server command to handle.
         */
        void HandleServerCommandWhisper(Message&& message) {
            // Ignore message unless it at least has a user name and message.
            if (message.parameters.size() < 2) {
                return;
            }

            // Extract whisper sender.
            const auto nickname = ExtractNicknameFromPrefix(message.prefix);
            WhisperInfo whisperInfo;
            whisperInfo.user = nickname;

            // Copy whisper message.
            whisperInfo.message = message.parameters[1];

            // Copy message tags.
            whisperInfo.tags = message.tags;

            // Trigger user callback.
            user->Whisper(std::move(whisperInfo));
        }

        /**
         * This method is called to handle the NOTICE command from the Twitch
         * server.
         *
         * @param[in] message
         *     This holds information about the server command to handle.
         */
        void HandleServerCommandNotice(Message&& message) {
            if (message.parameters.size() < 2) {
                return;
            }
            const auto& noticeText = message.parameters[1];
            NoticeInfo notice;
            notice.message = noticeText;
            if (message.parameters[0] != "*") {
                notice.channel = message.parameters[0].substr(1);
            }
            const auto idTag = message.tags.allTags.find("msg-id");
            if (idTag != message.tags.allTags.end()) {
                notice.id = idTag->second;
            }
            user->Notice(std::move(notice));
            if (
                !loggedIn
                && (
                    (noticeText == "Login unsuccessful")
                    || (noticeText == "Login authentication failed")
                )
            ) {
                user->LogOut();
                static const ActionProcessors loginFailActionProcessors = {
                    {Action::Type::AwaitMotd, &Impl::DiscardAction},
                };
                ProcessMessageWithAwaitingActions(
                    message,
                    loginFailActionProcessors
                );
            }
        }

        /**
         * This method is called to handle the HOSTTARGET command from the
         * Twitch server.
         *
         * @param[in] message
         *     This holds information about the server command to handle.
         */
        void HandleServerCommandHostTarget(Message&& message) {
            if (
                (message.parameters.size() < 2)
                || (message.parameters[0].length() < 2)
            ) {
                return;
            }
            HostInfo hostInfo;
            hostInfo.hosting = message.parameters[0].substr(1);
            const auto secondParameterParts = StringExtensions::Split(message.parameters[1], ' ');
            if (secondParameterParts[0] == "-") {
                hostInfo.on = false;
            } else {
                hostInfo.on = true;
                hostInfo.beingHosted = secondParameterParts[0];
            }
            if (
                sscanf(
                    secondParameterParts[1].c_str(),
                    "%zu",
                    &hostInfo.viewers
                ) != 1
            ) {
                hostInfo.viewers = 0;
            }
            user->Host(std::move(hostInfo));
        }

        /**
         * This method is called to handle the ROOMSTATE command from the
         * Twitch server.
         *
         * @param[in] message
         *     This holds information about the server command to handle.
         */
        void HandleServerCommandRoomState(Message&& message) {
            if (
                (message.parameters.size() < 1)
                || (message.parameters[0].length() < 2)
            ) {
                return;
            }
            for (const std::string& mode: { "slow", "followers-only", "r9k", "emote-only", "subs-only" }) {
                const auto modeTag = message.tags.allTags.find(mode);
                if (modeTag != message.tags.allTags.end()) {
                    RoomModeChangeInfo roomModeChange;
                    roomModeChange.channelName = message.parameters[0].substr(1);
                    roomModeChange.channelId = message.tags.channelId;
                    roomModeChange.mode = mode;
                    if (sscanf(modeTag->second.c_str(), "%d", &roomModeChange.parameter) != 1) {
                        roomModeChange.parameter = 0;
                    }
                    user->RoomModeChange(std::move(roomModeChange));
                }
            }
        }

        /**
         * This method is called to handle the CLEARCHAT command from the
         * Twitch server.
         *
         * @param[in] message
         *     This holds information about the server command to handle.
         */
        void HandleServerCommandClearChat(Message&& message) {
            // Ignore message unless it at least has a channel name.
            if (
                (message.parameters.size() < 1)
                || (message.parameters[0].length() < 2)
            ) {
                return;
            }

            // Extract channel name.
            ClearInfo clear;
            clear.channel = message.parameters[0].substr(1);

            // Interpret as clear-all or timeout/ban based on whether or not
            // there is an additional parameter (the target name).
            if (message.parameters.size() == 1) {
                clear.type = ClearInfo::Type::ClearAll;
            } else {
                // Extract user name.
                clear.user = message.parameters[1];

                // Extract ban/timeout reason, if any.
                const auto reasonTag = message.tags.allTags.find("ban-reason");
                if (reasonTag != message.tags.allTags.end()) {
                    clear.reason = UnescapeMessage(reasonTag->second);
                }

                // Check for ban duration; if none, it's a permanent ban,
                // rather than just a timeout.
                const auto banDurationTag = message.tags.allTags.find("ban-duration");
                if (banDurationTag == message.tags.allTags.end()) {
                    clear.type = ClearInfo::Type::Ban;
                } else {
                    clear.type = ClearInfo::Type::Timeout;

                    // Parse timeout duration.
                    if (sscanf(banDurationTag->second.c_str(), "%zu", &clear.duration) != 1) {
                        clear.duration = 0;
                    }
                }
            }

            // Copy message tags.
            clear.tags = message.tags;

            // Trigger callback to the user.
            user->Clear(std::move(clear));
        }

        /**
         * This method is called to handle the CLEARMSG command from the
         * Twitch server.
         *
         * @param[in] message
         *     This holds information about the server command to handle.
         */
        void HandleServerCommandClearMessage(Message&& message) {
            // Ignore message unless it at least has a channel name.
            if (
                (message.parameters.size() < 2)
                || (message.parameters[0].length() < 2)
            ) {
                return;
            }

            // Parse channel name.
            ClearInfo clear;
            clear.type = ClearInfo::Type::ClearMessage;
            clear.channel = message.parameters[0].substr(1);

            // Extract offending message content.
            clear.offendingMessageContent = message.parameters[1];

            // Extract offending message ID.
            const auto offendingMessageIdTag = message.tags.allTags.find("target-msg-id");
            if (offendingMessageIdTag != message.tags.allTags.end()) {
                clear.offendingMessageId = offendingMessageIdTag->second;
            }

            // Extract user name.
            const auto userNameTag = message.tags.allTags.find("login");
            if (userNameTag != message.tags.allTags.end()) {
                clear.user = userNameTag->second;
            }

            // Copy message tags.
            clear.tags = message.tags;

            // Trigger callback to the user.
            user->Clear(std::move(clear));
        }

        /**
         * This method is called to handle the MODE command from the
         * Twitch server.
         *
         * @param[in] message
         *     This holds information about the server command to handle.
         */
        void HandleServerCommandMode(Message&& message) {
            // Ignore message unless it at least has a channel name.
            if (
                (message.parameters.size() < 3)
                || (message.parameters[0].length() < 2)
                || (message.parameters[1].length() < 2)
            ) {
                return;
            }

            // Parse channel name.
            ModInfo mod;
            mod.channel = message.parameters[0].substr(1);

            // Determine whether modded or unmodded.
            if (message.parameters[1] == "-o") {
                mod.mod = false;
            } else if (message.parameters[1] == "+o") {
                mod.mod = true;
            } else {
                return;
            }

            // Extract user name.
            mod.user = message.parameters[2];

            // Trigger callback to the user.
            user->Mod(std::move(mod));
        }

        /**
         * This method is called to handle the GLOBALUSERSTATE command from the
         * Twitch server.
         *
         * @param[in] message
         *     This holds information about the server command to handle.
         */
        void HandleServerCommandGlobalUserState(Message&& message) {
            // Start off by saying this isn't a global state for the user.
            UserStateInfo userState;
            userState.global = true;

            // Copy tags.
            userState.tags = message.tags;

            // Trigger user callback.
            user->UserState(std::move(userState));
        }

        /**
         * This method is called to handle the USERSTATE command from the
         * Twitch server.
         *
         * @param[in] message
         *     This holds information about the server command to handle.
         */
        void HandleServerCommandUserState(Message&& message) {
            // Ignore message unless it at least has a channel name.
            if (
                (message.parameters.size() < 1)
                || (message.parameters[0].length() < 2)
            ) {
                return;
            }

            // Start off by saying this isn't a global state for the user.
            UserStateInfo userState;
            userState.global = false;

            // Parse channel name.
            userState.channel = message.parameters[0].substr(1);

            // Copy tags.
            userState.tags = message.tags;

            // Trigger user callback.
            user->UserState(std::move(userState));
        }

        /**
         * This method is called to handle the RECONNECT command from the
         * Twitch server.
         *
         * @param[in] message
         *     This holds information about the server command to handle.
         */
        void HandleServerCommandReconnect(Message&& message) {
            user->Doom();
        }

        /**
         * This method is called to handle the USERNOTICE command from the
         * Twitch server.
         *
         * @param[in] message
         *     This holds information about the server command to handle.
         */
        void HandleServerCommandUserNotice(Message&& message) {
            // Ignore message unless it at least has a channel name.
            if (
                (message.parameters.size() < 1)
                || (message.parameters[0].length() < 2)
            ) {
                return;
            }

            // This may be a ritual, a raid, or a sub notification, based
            // on the message ID.
            const auto messageIdTag = message.tags.allTags.find("msg-id");
            if (messageIdTag == message.tags.allTags.end()) {
                return;
            }
            const auto messageId = messageIdTag->second;
            if (messageId == "ritual") {
                // Extract channel name.
                RitualInfo ritual;
                ritual.channel = message.parameters[0].substr(1);

                // Extract user name.
                const auto userNameTag = message.tags.allTags.find("login");
                if (userNameTag != message.tags.allTags.end()) {
                    ritual.user = userNameTag->second;
                }

                // Extract ritual name.
                const auto ritualTag = message.tags.allTags.find("msg-param-ritual-name");
                if (ritualTag != message.tags.allTags.end()) {
                    ritual.ritual = ritualTag->second;
                }

                // Extract system message.
                const auto systemMessageTag = message.tags.allTags.find("system-msg");
                if (systemMessageTag != message.tags.allTags.end()) {
                    ritual.systemMessage = UnescapeMessage(systemMessageTag->second);
                }

                // Copy over the tags.
                ritual.tags = message.tags;

                // Trigger callback to the user.
                user->Ritual(std::move(ritual));
            } else if (messageId == "raid") {
                // Extract channel name.
                RaidInfo raid;
                raid.channel = message.parameters[0].substr(1);

                // Extract raider name.
                const auto userNameTag = message.tags.allTags.find("login");
                if (userNameTag != message.tags.allTags.end()) {
                    raid.raider = userNameTag->second;
                }

                // Extract system message.
                const auto systemMessageTag = message.tags.allTags.find("system-msg");
                if (systemMessageTag != message.tags.allTags.end()) {
                    raid.systemMessage = UnescapeMessage(systemMessageTag->second);
                }

                // Parse viewer count.
                const auto viewerCountTag = message.tags.allTags.find("msg-param-viewerCount");
                if (
                    (viewerCountTag == message.tags.allTags.end())
                    || (sscanf(viewerCountTag->second.c_str(), "%zu", &raid.viewers) != 1)
                ) {
                    raid.viewers = 0;
                }

                // Copy over the tags.
                raid.tags = message.tags;

                // Trigger callback to the user.
                user->Raid(std::move(raid));
            } else {
                // Extract channel name.
                SubInfo sub;
                sub.channel = message.parameters[0].substr(1);

                // Extract user name.
                const auto userNameTag = message.tags.allTags.find("login");
                if (userNameTag != message.tags.allTags.end()) {
                    sub.user = userNameTag->second;
                }

                // Extract user message, if any.
                if (message.parameters.size() >= 2) {
                    sub.userMessage = message.parameters[1];
                }

                // Extract system message.
                const auto systemMessageTag = message.tags.allTags.find("system-msg");
                if (systemMessageTag != message.tags.allTags.end()) {
                    sub.systemMessage = UnescapeMessage(systemMessageTag->second);
                }

                // Extract subscription type.
                const auto subTypeTag = message.tags.allTags.find("msg-id");
                if (subTypeTag != message.tags.allTags.end()) {
                    const auto& subType = subTypeTag->second;
                    if (subType == "sub") {
                        sub.type = SubInfo::Type::Sub;
                    } else if (subType == "resub") {
                        sub.type = SubInfo::Type::Resub;

                        // Parse subscription renewal month count.
                        const auto monthsIdTag = message.tags.allTags.find("msg-param-months");
                        if (
                            (monthsIdTag == message.tags.allTags.end())
                            || (sscanf(monthsIdTag->second.c_str(), "%zu", &sub.months) != 1)
                        ) {
                            sub.months = 0;
                        }
                    } else if (subType == "subgift") {
                        sub.type = SubInfo::Type::Gifted;

                        // Extract recipient display name.
                        const auto recipientDisplayNameTag = message.tags.allTags.find("msg-param-recipient-display-name");
                        if (recipientDisplayNameTag != message.tags.allTags.end()) {
                            sub.recipientDisplayName = recipientDisplayNameTag->second;
                        }

                        // Extract recipient user name.
                        const auto recipientUserNameTag = message.tags.allTags.find("msg-param-recipient-user-name");
                        if (recipientUserNameTag != message.tags.allTags.end()) {
                            sub.recipientUserName = recipientUserNameTag->second;
                        }

                        // Parse recipient ID.
                        const auto recipientIdTag = message.tags.allTags.find("msg-param-recipient-id");
                        if (
                            (recipientIdTag == message.tags.allTags.end())
                            || (sscanf(recipientIdTag->second.c_str(), "%" SCNuMAX, &sub.recipientId) != 1)
                        ) {
                            sub.recipientId = 0;
                        }

                        // Parse sender gift count.
                        const auto senderCountTag = message.tags.allTags.find("msg-param-sender-count");
                        if (
                            (senderCountTag == message.tags.allTags.end())
                            || (sscanf(senderCountTag->second.c_str(), "%zu", &sub.senderCount) != 1)
                        ) {
                            sub.senderCount = 0;
                        }
                    } else if (subType == "submysterygift") {
                        sub.type = SubInfo::Type::MysteryGift;

                        // Parse gift count.
                        const auto recipientIdTag = message.tags.allTags.find("msg-param-mass-gift-count");
                        if (
                            (recipientIdTag == message.tags.allTags.end())
                            || (sscanf(recipientIdTag->second.c_str(), "%zu", &sub.massGiftCount) != 1)
                        ) {
                            sub.massGiftCount = 0;
                        }

                        // Parse sender gift count.
                        const auto senderCountTag = message.tags.allTags.find("msg-param-sender-count");
                        if (
                            (senderCountTag == message.tags.allTags.end())
                            || (sscanf(senderCountTag->second.c_str(), "%zu", &sub.senderCount) != 1)
                        ) {
                            sub.senderCount = 0;
                        }
                    }
                }

                // Extract plan name.
                const auto planNameTag = message.tags.allTags.find("msg-param-sub-plan-name");
                if (planNameTag != message.tags.allTags.end()) {
                    sub.planName = UnescapeMessage(planNameTag->second);
                }

                // Parse plan ID.
                const auto planIdTag = message.tags.allTags.find("msg-param-sub-plan");
                if (
                    (planIdTag == message.tags.allTags.end())
                    || (sscanf(planIdTag->second.c_str(), "%" SCNuMAX, &sub.planId) != 1)
                ) {
                    sub.planId = 0;
                }

                // Copy over the tags.
                sub.tags = message.tags;

                // Trigger callback to the user.
                user->Sub(std::move(sub));
            }
        }

        /**
         * This method performs the given ServerDisconnected action.
         *
         * @param[in] action
         *     This is the action to perform.
         */
        void PerformActionServerDisconnected(Action&& action) {
            Disconnect();
        }

        /**
         * This method performs the given Join action.
         *
         * @param[in] action
         *     This is the action to perform.
         */
        void PerformActionJoin(Action&& action) {
            if (connection == nullptr) {
                return;
            }
            SendLineToTwitchServer(*connection, "JOIN #" + action.nickname);
        }

        /**
         * This method performs the given Leave action.
         *
         * @param[in] action
         *     This is the action to perform.
         */
        void PerformActionLeave(Action&& action) {
            if (connection == nullptr) {
                return;
            }
            SendLineToTwitchServer(*connection, "PART #" + action.nickname);
        }

        /**
         * This method performs the given SendMessage action.
         *
         * @param[in] action
         *     This is the action to perform.
         */
        void PerformActionSendMessage(Action&& action) {
            if (connection == nullptr) {
                return;
            }
            if (anonymous) {
                return;
            }
            if (action.parent.empty()) {
                SendLineToTwitchServer(*connection, "PRIVMSG #" + action.nickname + " :" + action.message);
            } else {
                SendLineToTwitchServer(*connection, "@reply-parent-msg-id=" + action.parent + " PRIVMSG #" + action.nickname + " :" + action.message);
            }
        }

        /**
         * This method performs the given SendWhisper action.
         *
         * @param[in] action
         *     This is the action to perform.
         */
        void PerformActionSendWhisper(Action&& action) {
            if (connection == nullptr) {
                return;
            }
            if (anonymous) {
                return;
            }
            SendLineToTwitchServer(*connection, "PRIVMSG #jtv :.w " + action.nickname + " " + action.message);
        }

        /**
         * This method signals the worker thread to stop.
         */
        void StopWorker() {
            std::lock_guard< decltype(mutex) > lock(mutex);
            stopWorker = true;
            wakeWorker.notify_one();
        }

        /**
         * This runs in its own thread and performs background tasks
         * for the object.
         */
        void Worker() {
            std::unique_lock< decltype(mutex) > lock(mutex);
            while (!stopWorker) {
                lock.unlock();
                if (timeKeeper != nullptr) {
                    ProcessTimeouts();
                }
                lock.lock();
                while (!actionsToBePerformed.empty()) {
                    auto action = actionsToBePerformed.front();
                    actionsToBePerformed.pop_front();
                    lock.unlock();
                    PerformAction(std::move(action));
                    lock.lock();
                }
                if (!connection) {
                    actionsAwaitingResponses.clear();
                }
                if (!actionsAwaitingResponses.empty()) {
                    wakeWorker.wait_for(
                        lock,
                        std::chrono::milliseconds(50),
                        [this]{
                            return (
                                stopWorker
                                || !actionsToBePerformed.empty()
                            );
                        }
                    );
                } else {
                    wakeWorker.wait(
                        lock,
                        [this]{
                            return (
                                stopWorker
                                || !actionsToBePerformed.empty()
                            );
                        }
                    );
                }
            }
        }
    };

    Messaging::~Messaging() noexcept {
        impl_->StopWorker();
        impl_->worker.join();
    }

    Messaging::Messaging()
        : impl_ (new Impl())
    {
        impl_->worker = std::thread(&Impl::Worker, impl_.get());
    }

    SystemAbstractions::DiagnosticsSender::UnsubscribeDelegate Messaging::SubscribeToDiagnostics(
        SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate delegate,
        size_t minLevel
    ) {
        return impl_->diagnosticsSender.SubscribeToDiagnostics(delegate, minLevel);
    }

    void Messaging::SetConnectionFactory(ConnectionFactory connectionFactory) {
        impl_->connectionFactory = connectionFactory;
    }

    void Messaging::SetTimeKeeper(std::shared_ptr< TimeKeeper > timeKeeper) {
        impl_->timeKeeper = timeKeeper;
    }

    void Messaging::SetUser(std::shared_ptr< User > user) {
        impl_->user = user;
    }

    void Messaging::LogIn(
        const std::string& nickname,
        const std::string& token
    ) {
        Action action;
        action.type = Action::Type::LogIn;
        action.nickname = nickname;
        action.token = token;
        action.anonymous = false;
        impl_->PostAction(std::move(action));
    }

    void Messaging::LogInAnonymously() {
        Action action;
        action.type = Action::Type::LogIn;
        action.nickname = StringExtensions::sprintf(
            "justinfan%d",
            rand()
        );
        action.anonymous = true;
        impl_->PostAction(std::move(action));
    }

    void Messaging::LogOut(const std::string& farewell) {
        Action action;
        action.type = Action::Type::LogOut;
        action.message = farewell;
        impl_->PostAction(std::move(action));
    }

    void Messaging::Join(const std::string& channel) {
        Action action;
        action.type = Action::Type::Join;
        action.nickname = channel;
        impl_->PostAction(std::move(action));
    }

    void Messaging::Leave(const std::string& channel) {
        Action action;
        action.type = Action::Type::Leave;
        action.nickname = channel;
        impl_->PostAction(std::move(action));
    }

    void Messaging::SendMessage(
        const std::string& channel,
        const std::string& message
    ) {
        Action action;
        action.type = Action::Type::SendMessage;
        action.nickname = channel;
        action.message = message;
        impl_->PostAction(std::move(action));
    }

    void Messaging::SendResponse(
        const std::string& channel,
        const std::string& message,
        const std::string& parent
    ) {
        Action action;
        action.type = Action::Type::SendMessage;
        action.nickname = channel;
        action.message = message;
        action.parent = parent;
        impl_->PostAction(std::move(action));
    }

    void Messaging::SendWhisper(
        const std::string& nickname,
        const std::string& message
    ) {
        Action action;
        action.type = Action::Type::SendWhisper;
        action.nickname = nickname;
        action.message = message;
        impl_->PostAction(std::move(action));
    }

}
