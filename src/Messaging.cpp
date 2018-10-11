/**
 * @file Messaging.cpp
 *
 * This module contains the implementation of the
 * Twitch::Messaging class.
 *
 * © 2016-2018 by Richard Walters
 */

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <list>
#include <mutex>
#include <string>
#include <SystemAbstractions/StringExtensions.hpp>
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
     * These are the states in which the Messaging class can be.
     */
    enum class State {
        /**
         * The client has either not yet logged in, or has logged out.
         * There is no active connection.
         */
        NotLoggedIn,

        /**
         * The client has completely logged into the server,
         * with an active connection.
         */
        LoggedIn,
    };

    /**
     * These are the types of actions which the Messaging class worker can
     * perform.
     */
    enum class ActionType {
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
    };

    /**
     * This is used to convey an action for the Messaging class worker
     * to perform, including any parameters.
     */
    struct Action {
        /**
         * This is the type of action to perform.
         */
        ActionType type;

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
         * This is the time, according to the time keeper, at which
         * the action will be considered as having timed out.
         */
        double expiration = 0.0;
    };

    /**
     * This contains all the information parsed from a single message
     * from the Twitch server.
     */
    struct Message {
        /**
         * If this is not an empty string, the message included a prefix,
         * which is stored here, without the leading colon (:) character.
         */
        std::string prefix;

        /**
         * This is the command portion of the message, which may be
         * a three-digit code, or an IRC command name.
         *
         * If it's empty, the message was invalid, or there was no message.
         */
        std::string command;

        /**
         * These are the parameters, if any, provided in the message.
         */
        std::vector< std::string > parameters;
    };

}

namespace Twitch {

    /**
     * This contains the private properties of a Messaging instance.
     */
    struct Messaging::Impl {
        // Properties

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
         * This is used to synchronize access to the object.
         */
        std::mutex mutex;

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
         * This is used to perform background tasks for the object.
         */
        std::thread worker;

        /**
         * These are the actions to be performed by the worker thread.
         */
        std::deque< Action > actionsToBePerformed;

        // Methods

        /**
         * This is the constructor for the structure.
         */
        Impl()
            : diagnosticsSender("TMI")
        {
        }

        /**
         * This method extracts the next message received from the
         * Twitch server.
         *
         * @param[in,out] dataReceived
         *      This is essentially just a buffer to receive raw characters
         *      from the Twitch server, until a complete line has been
         *      received, removed from this buffer, and handled appropriately.
         *
         * @param[out] message
         *     This is where to store the next message received from the
         *     Twitch server.
         *
         * @return
         *     An indication of whether or not a complete line was
         *     extracted is returned.
         */
        bool GetNextMessage(
            std::string& dataReceived,
            Message& message
        ) {
            // This tracks the current state of the state machine used
            // in this function to parse the raw text of the message.
            enum class State {
                LineFirstCharacter,
                Prefix,
                CommandFirstCharacter,
                CommandNotFirstCharacter,
                ParameterFirstCharacter,
                ParameterNotFirstCharacter,
                Trailer,
            } state = State::LineFirstCharacter;

            // Extract the next line.
            const auto lineEnd = dataReceived.find(CRLF);
            if (lineEnd == std::string::npos) {
                return false;
            }
            const auto line = dataReceived.substr(0, lineEnd);
            diagnosticsSender.SendDiagnosticInformationString(0, "> " + line);

            // Remove the line from the buffer.
            dataReceived = dataReceived.substr(lineEnd + CRLF.length());

            // Unpack the message from the line.
            size_t offset = 0;
            message = Message();
            while (offset < line.length()) {
                switch (state) {
                    // First character of the line.  It could be ':',
                    // which signals a prefix, or it's the first character
                    // of the command.
                    case State::LineFirstCharacter: {
                        if (line[offset] == ':') {
                            state = State::Prefix;
                        } else {
                            state = State::CommandNotFirstCharacter;
                            message.command += line[offset];
                        }
                    } break;

                    // Prefix
                    case State::Prefix: {
                        if (line[offset] == ' ') {
                            state = State::CommandFirstCharacter;
                        } else {
                            message.prefix += line[offset];
                        }
                    } break;

                    // First character of command
                    case State::CommandFirstCharacter: {
                        if (line[offset] != ' ') {
                            state = State::CommandNotFirstCharacter;
                            message.command += line[offset];
                        }
                    } break;

                    // Command
                    case State::CommandNotFirstCharacter: {
                        if (line[offset] == ' ') {
                            state = State::ParameterFirstCharacter;
                        } else {
                            message.command += line[offset];
                        }
                    } break;

                    // First character of parameter
                    case State::ParameterFirstCharacter: {
                        if (line[offset] == ':') {
                            state = State::Trailer;
                            message.parameters.push_back("");
                        } else if (line[offset] != ' ') {
                            state = State::ParameterNotFirstCharacter;
                            message.parameters.push_back(line.substr(offset, 1));
                        }
                    } break;

                    // Parameter (not last, or last having no spaces)
                    case State::ParameterNotFirstCharacter: {
                        if (line[offset] == ' ') {
                            state = State::ParameterFirstCharacter;
                        } else {
                            message.parameters.back() += line[offset];
                        }
                    } break;

                    // Last Parameter (may include spaces)
                    case State::Trailer: {
                        message.parameters.back() += line[offset];
                    } break;
                }
                ++offset;
            }
            if (
                (state == State::LineFirstCharacter)
                || (state == State::Prefix)
                || (state == State::CommandFirstCharacter)
            ) {
                message.command.clear();
            }
            return true;
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
        void MessageReceived(const std::string& rawText) {
            std::lock_guard< decltype(mutex) > lock(mutex);
            Action action;
            action.type = ActionType::ProcessMessagesReceived;
            action.message = rawText;
            actionsToBePerformed.push_back(action);
            wakeWorker.notify_one();
        }

        /**
         * This method is called when the Twitch server closes its end of the
         * connection.
         */
        void ServerDisconnected() {
            std::lock_guard< decltype(mutex) > lock(mutex);
            Action action;
            action.type = ActionType::ServerDisconnected;
            actionsToBePerformed.push_back(action);
            wakeWorker.notify_one();
        }

        /**
         * This method is called whenever the user agent disconnects from the
         * Twitch server.
         *
         * @param[in,out] connection
         *     This is the connection to disconnect.
         *
         * @param[in] farewell
         *     If not empty, the user agent should sent a QUIT command before
         *     disconnecting, and this is a message to include in the
         *     QUIT command.
         */
        void Disconnect(
            Connection& connection,
            const std::string farewell = ""
        ) {
            if (!farewell.empty()) {
                connection.Send("QUIT :" + farewell + CRLF);
            }
            connection.Disconnect();
            user->LogOut();
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
            // This is the interface to the current connection to the Twitch
            // server, if we are connected.
            std::shared_ptr< Connection > connection;

            // This is essentially just a buffer to receive raw characters from
            // the Twitch server, until a complete line has been received,
            // removed from this buffer, and handled appropriately.
            std::string dataReceived;

            // This flag indicates whether or not the client has finished
            // logging into the Twitch server (we've received the Message Of
            // The Day (MOTD) from the server).
            bool loggedIn = false;

            // This holds onto any actions for which the worker is awaiting a
            // response from the server.
            std::list< Action > actionsAwaitingResponses;

            // This keeps track of the current state of the connection.
            State state = State::NotLoggedIn;

            // This is the nickname to use when logging in.
            std::string nickname;

            // This is the OAuth token to be used to authenticate
            // with the server.
            std::string token;

            // These are the IRCv3 capabilities advertised by the server.
            std::string capsSupported;

            std::unique_lock< decltype(mutex) > lock(mutex);
            while (!stopWorker) {
                lock.unlock();
                if (timeKeeper != nullptr) {
                    const auto now = timeKeeper->GetCurrentTime();
                    for (
                        auto it = actionsAwaitingResponses.begin(),
                        end = actionsAwaitingResponses.end();
                        it != end;
                    ) {
                        const auto& action = *it;
                        if (now >= action.expiration) {
                            switch (action.type) {
                                case ActionType::LogIn: {
                                    Disconnect(*connection, "Timeout waiting for capability list");
                                } break;

                                case ActionType::RequestCaps: {
                                    Disconnect(*connection, "Timeout waiting for response to capability request");
                                } break;

                                case ActionType::AwaitMotd: {
                                    Disconnect(*connection, "Timeout waiting for MOTD");
                                } break;

                                default: {
                                } break;
                            }
                            it = actionsAwaitingResponses.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }
                lock.lock();
                while (!actionsToBePerformed.empty()) {
                    auto action = actionsToBePerformed.front();
                    actionsToBePerformed.pop_front();
                    lock.unlock();
                    switch (action.type) {
                        case ActionType::LogIn: {
                            if (connection != nullptr) {
                                break;
                            }
                            connection = connectionFactory();
                            connection->SetMessageReceivedDelegate(
                                std::bind(&Impl::MessageReceived, this, std::placeholders::_1)
                            );
                            connection->SetDisconnectedDelegate(
                                std::bind(&Impl::ServerDisconnected, this)
                            );
                            if (connection->Connect()) {
                                capsSupported.clear();
                                SendLineToTwitchServer(*connection, "CAP LS 302");
                                nickname = action.nickname;
                                token = action.token;
                                if (timeKeeper != nullptr) {
                                    action.expiration = timeKeeper->GetCurrentTime() + LOG_IN_TIMEOUT_SECONDS;
                                }
                                actionsAwaitingResponses.push_back(std::move(action));
                            } else {
                                user->LogOut();
                            }
                        } break;

                        case ActionType::LogOut: {
                            if (connection != nullptr) {
                                Disconnect(*connection, action.message);
                            }
                        } break;

                        case ActionType::ProcessMessagesReceived: {
                            dataReceived += action.message;
                            Message message;
                            while (GetNextMessage(dataReceived, message)) {
                                if (message.command.empty()) {
                                    continue;
                                }
                                if (message.command == "376") { // RPL_ENDOFMOTD (RFC 1459)
                                    for (
                                        auto it = actionsAwaitingResponses.begin(),
                                        end = actionsAwaitingResponses.end();
                                        it != end;
                                    ) {
                                        auto action = *it;
                                        bool handled = false;
                                        switch (action.type) {
                                            case ActionType::AwaitMotd: {
                                                handled = true;
                                                if (!loggedIn) {
                                                    loggedIn = true;
                                                    user->LogIn();
                                                }
                                                state = State::LoggedIn;
                                            } break;

                                            default: {
                                            } break;
                                        }
                                        if (handled) {
                                            it = actionsAwaitingResponses.erase(it);
                                        } else {
                                            ++it;
                                        }
                                    }
                                } else if (message.command == "PING") {
                                    if (message.parameters.size() < 1) {
                                        continue;
                                    }
                                    const auto server = message.parameters[0];
                                    if (connection != nullptr) {
                                        SendLineToTwitchServer(*connection, "PONG :" + server);
                                    }
                                } else if (message.command == "JOIN") {
                                    if (
                                        (message.parameters.size() < 1)
                                        && (message.parameters[0].length() < 2)
                                    ) {
                                        continue;
                                    }
                                    const auto nicknameDelimiter = message.prefix.find('!');
                                    if (nicknameDelimiter == std::string::npos) {
                                        continue;
                                    }
                                    const auto nickname = message.prefix.substr(0, nicknameDelimiter);
                                    const auto channel = message.parameters[0].substr(1);
                                    user->Join(channel, nickname);
                                } else if (message.command == "PART") {
                                    if (
                                        (message.parameters.size() < 1)
                                        && (message.parameters[0].length() < 2)
                                    ) {
                                        continue;
                                    }
                                    const auto nicknameDelimiter = message.prefix.find('!');
                                    if (nicknameDelimiter == std::string::npos) {
                                        continue;
                                    }
                                    const auto nickname = message.prefix.substr(0, nicknameDelimiter);
                                    const auto channel = message.parameters[0].substr(1);
                                    user->Leave(channel, nickname);
                                } else if (message.command == "PRIVMSG") {
                                    if (
                                        (message.parameters.size() < 2)
                                        && (message.parameters[0].length() < 2)
                                    ) {
                                        continue;
                                    }
                                    const auto nicknameDelimiter = message.prefix.find('!');
                                    if (nicknameDelimiter == std::string::npos) {
                                        continue;
                                    }
                                    MessageInfo messageInfo;
                                    messageInfo.user = message.prefix.substr(0, nicknameDelimiter);
                                    messageInfo.channel = message.parameters[0].substr(1);
                                    messageInfo.message = message.parameters[1];
                                    user->Message(std::move(messageInfo));
                                } else if (message.command == "CAP") {
                                    for (
                                        auto it = actionsAwaitingResponses.begin(),
                                        end = actionsAwaitingResponses.end();
                                        it != end;
                                    ) {
                                        auto action = *it;
                                        bool handled = false;
                                        switch (action.type) {
                                            case ActionType::LogIn: {
                                                if (
                                                    (message.parameters.size() < 3)
                                                    || (message.parameters[1] != "LS")
                                                ) {
                                                    break;
                                                }
                                                if (!capsSupported.empty()) {
                                                    capsSupported += " ";
                                                }
                                                if (message.parameters[2] == "*") {
                                                    capsSupported += message.parameters[3];
                                                } else {
                                                    handled = true;
                                                    capsSupported += message.parameters[2];
                                                    const auto capsSupportedVector = SystemAbstractions::Split(capsSupported, ' ');
                                                    if (
                                                        std::find(
                                                            capsSupportedVector.begin(),
                                                            capsSupportedVector.end(),
                                                            "twitch.tv/commands"
                                                        ) == capsSupportedVector.end()
                                                    ) {
                                                        SendLineToTwitchServer(*connection, "CAP END");
                                                        SendLineToTwitchServer(*connection, "PASS oauth:" + token);
                                                        SendLineToTwitchServer(*connection, "NICK " + nickname);
                                                        action.type = ActionType::AwaitMotd;
                                                        if (timeKeeper != nullptr) {
                                                            action.expiration = timeKeeper->GetCurrentTime() + LOG_IN_TIMEOUT_SECONDS;
                                                        }
                                                        actionsAwaitingResponses.push_back(std::move(action));
                                                    } else {
                                                        SendLineToTwitchServer(*connection, "CAP REQ :twitch.tv/commands");
                                                        action.type = ActionType::RequestCaps;
                                                        if (timeKeeper != nullptr) {
                                                            action.expiration = timeKeeper->GetCurrentTime() + LOG_IN_TIMEOUT_SECONDS;
                                                        }
                                                        actionsAwaitingResponses.push_back(std::move(action));
                                                    }
                                                }
                                            } break;

                                            case ActionType::RequestCaps: {
                                                if (
                                                    (message.parameters.size() < 2)
                                                    || (
                                                        (message.parameters[1] != "ACK")
                                                        && (message.parameters[1] != "NAK")
                                                    )
                                                ) {
                                                    break;
                                                }
                                                handled = true;
                                                SendLineToTwitchServer(*connection, "CAP END");
                                                SendLineToTwitchServer(*connection, "PASS oauth:" + token);
                                                SendLineToTwitchServer(*connection, "NICK " + nickname);
                                                action.type = ActionType::AwaitMotd;
                                                if (timeKeeper != nullptr) {
                                                    action.expiration = timeKeeper->GetCurrentTime() + LOG_IN_TIMEOUT_SECONDS;
                                                }
                                                actionsAwaitingResponses.push_back(std::move(action));
                                            } break;

                                            default: {
                                            } break;
                                        }
                                        if (handled) {
                                            it = actionsAwaitingResponses.erase(it);
                                        } else {
                                            ++it;
                                        }
                                    }
                                }
                            }
                        } break;

                        case ActionType::ServerDisconnected: {
                            Disconnect(*connection);
                        } break;

                        case ActionType::Join: {
                            if (connection == nullptr) {
                                break;
                            }
                            SendLineToTwitchServer(*connection, "JOIN #" + action.nickname);
                        } break;

                        case ActionType::Leave: {
                            if (connection == nullptr) {
                                break;
                            }
                            SendLineToTwitchServer(*connection, "PART #" + action.nickname);
                        } break;

                        case ActionType::SendMessage: {
                            if (connection == nullptr) {
                                break;
                            }
                            SendLineToTwitchServer(*connection, "PRIVMSG #" + action.nickname + " :" + action.message);
                        } break;

                        default: {
                        } break;
                    }
                    lock.lock();
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

    Messaging::~Messaging() {
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
        std::lock_guard< decltype(impl_->mutex) > lock(impl_->mutex);
        Action action;
        action.type = ActionType::LogIn;
        action.nickname = nickname;
        action.token = token;
        impl_->actionsToBePerformed.push_back(action);
        impl_->wakeWorker.notify_one();
    }

    void Messaging::LogOut(const std::string& farewell) {
        std::lock_guard< decltype(impl_->mutex) > lock(impl_->mutex);
        Action action;
        action.type = ActionType::LogOut;
        action.message = farewell;
        impl_->actionsToBePerformed.push_back(action);
        impl_->wakeWorker.notify_one();
    }

    void Messaging::Join(const std::string& channel) {
        std::lock_guard< decltype(impl_->mutex) > lock(impl_->mutex);
        Action action;
        action.type = ActionType::Join;
        action.nickname = channel;
        impl_->actionsToBePerformed.push_back(action);
        impl_->wakeWorker.notify_one();
    }

    void Messaging::Leave(const std::string& channel) {
        std::lock_guard< decltype(impl_->mutex) > lock(impl_->mutex);
        Action action;
        action.type = ActionType::Leave;
        action.nickname = channel;
        impl_->actionsToBePerformed.push_back(action);
        impl_->wakeWorker.notify_one();
    }

    void Messaging::SendMessage(
        const std::string& channel,
        const std::string& message
    ) {
        std::lock_guard< decltype(impl_->mutex) > lock(impl_->mutex);
        Action action;
        action.type = ActionType::SendMessage;
        action.nickname = channel;
        action.message = message;
        impl_->actionsToBePerformed.push_back(action);
        impl_->wakeWorker.notify_one();
    }

}
