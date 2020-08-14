/**
 * @file Message.cpp
 *
 * This module contains the implementation of the Twitch::Message structure.
 *
 * © 2018 by Richard Walters
 */

#include "Message.hpp"

#include <inttypes.h>
#include <stdio.h>
#include <StringExtensions/StringExtensions.hpp>
#include <utility>

namespace {

    /**
     * This is the required line terminator for lines of text
     * sent to or from Twitch chat servers.
     */
    const std::string CRLF = "\r\n";

    /**
     * This function breaks the given string into two strings at the first
     * position of an unescaped equal sign.
     *
     * @param[in] s
     *     This is the string to split.
     *
     * @return
     *     The two pieces of the input string on either side of the first
     *     unescaped equal sign is returned.
     */
    std::pair< std::string, std::string > SplitNameValue(const std::string& s) {
        bool escape = false;
        for (size_t i = 0; i < s.length(); ++i) {
            if (escape) {
                escape = false;
            } else {
                if (s[i] == '\\') {
                    escape = true;
                } else if (s[i] == '=') {
                    return {
                        s.substr(0, i),
                        s.substr(i + 1)
                    };
                }
            }
        }
        return {s, ""};
    }

    /**
     * This is a helper function which parses the tags string from a raw Twitch
     * message and stores them in the given message.
     *
     * @param[in] unparsedTags
     *     This is the raw string containing the tags for the given message.
     *
     * @return
     *     The tags parsed from the given raw tags string is returned.
     */
    Twitch::Messaging::TagsInfo ParseTags(const std::string& unparsedTags) {
        Twitch::Messaging::TagsInfo parsedTags;
        const auto tags = StringExtensions::Split(unparsedTags, ';');
        for (const auto& tag: tags) {
            const auto nameValuePair = SplitNameValue(tag);
            const auto& name = nameValuePair.first;
            const auto& value = nameValuePair.second;
            parsedTags.allTags[name] = value;
            if (name == "badges") {
                const auto badges = StringExtensions::Split(value, ',');
                for (const auto& badge: badges) {
                    (void)parsedTags.badges.insert(badge);
                }
            } else if (name == "color") {
                (void)sscanf(
                    value.c_str(),
                    "#%" SCNx32,
                    &parsedTags.color
                );
            } else if (name == "display-name") {
                parsedTags.displayName = value;
            } else if (name == "emotes") {
                const auto emotes = StringExtensions::Split(value, '/');
                for (const auto& emote: emotes) {
                    const auto idInstancesPair = StringExtensions::Split(emote, ':');
                    if (idInstancesPair.size() != 2) {
                        continue;
                    }
                    int id;
                    if (sscanf(idInstancesPair[0].c_str(), "%d", &id) != 1) {
                        continue;
                    }
                    auto& emoteInstances = parsedTags.emotes[id];
                    const auto instances = StringExtensions::Split(idInstancesPair[1], ',');
                    for (const auto& instance: instances) {
                        int begin, end;
                        if (sscanf(instance.c_str(), "%d-%d", &begin, &end) != 2) {
                            continue;
                        }
                        emoteInstances.push_back({begin, end});
                    }
                }
            } else if (name == "tmi-sent-ts") {
                uintmax_t timeAsInt;
                if (sscanf(value.c_str(), "%" SCNuMAX, &timeAsInt) == 1) {
                    parsedTags.timestamp = (decltype(parsedTags.timestamp))(timeAsInt / 1000);
                    parsedTags.timeMilliseconds = (decltype(parsedTags.timeMilliseconds))(timeAsInt % 1000);
                } else {
                    parsedTags.timestamp = 0;
                    parsedTags.timeMilliseconds = 0;
                }
            } else if (name == "room-id") {
                if (sscanf(value.c_str(), "%" SCNuMAX, &parsedTags.channelId) != 1) {
                    parsedTags.channelId = 0;
                }
            } else if (
                (name == "user-id")
                || (name == "target-user-id")
            ) {
                if (sscanf(value.c_str(), "%" SCNuMAX, &parsedTags.userId) != 1) {
                    parsedTags.userId = 0;
                }
            } else if (name == "id") {
                parsedTags.id = value;
            }
        }
        return parsedTags;
    }

}

namespace Twitch {

    bool Message::Parse(
        std::string& dataReceived,
        Message& message,
        SystemAbstractions::DiagnosticsSender& diagnosticsSender
    ) {
        // This tracks the current state of the state machine used
        // in this function to parse the raw text of the message.
        enum class State {
            LineFirstCharacter,
            Tags,
            PrefixOrCommandFirstCharacter,
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
        std::string unparsedTags;
        while (offset < line.length()) {
            switch (state) {
                // First character of the line.  It could be ':',
                // which signals a prefix, or it's the first character
                // of the command.
                case State::LineFirstCharacter: {
                    if (line[offset] == '@') {
                        state = State::Tags;
                    } else if (line[offset] == ':') {
                        state = State::Prefix;
                    } else {
                        state = State::CommandNotFirstCharacter;
                        message.command += line[offset];
                    }
                } break;

                // Tags
                case State::Tags: {
                    if (line[offset] == ' ') {
                        state = State::PrefixOrCommandFirstCharacter;
                    } else {
                        unparsedTags += line[offset];
                    }
                } break;

                // Prefix marker or first character of command
                case State::PrefixOrCommandFirstCharacter: {
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
            || (state == State::Tags)
            || (state == State::PrefixOrCommandFirstCharacter)
            || (state == State::Prefix)
            || (state == State::CommandFirstCharacter)
        ) {
            message.command.clear();
        }
        message.tags = ParseTags(unparsedTags);
        return true;
    }

}
