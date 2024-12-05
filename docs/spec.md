# Buxtehude Specification

Buxtehude is an interprocess communication protocol implemented as a library designed for Fitsch.

## Protocol

### Binary format

Buxtehude messages shall be transmitted over a byte-stream in the following format:

Field|Length|Description
---|---|---
Format|1 byte|`0x00` = JSON, `0x01` = MessagePack
Length|4 bytes|Length of the message in bytes (x)
Content|x bytes|Valid JSON or MessagePack

### Message fields

The message may contain the following recognised fields:

Field|Description
---|---
type|The message type. This is the only obligatory field. `$$error`, `$$disconnect`, `$$handshake`, `$$available`, `$$connect` shall be reserved keywords.
src|Teamname of the origin of the message. `$$server` shall be a reserved keyword.
dest|Teamname of the destination clients of the message. `$$server`, `$$all` and `$$you` shall be reserved keywords.
only_first|Whether to send this message to only the first available client under the destination teamname. The absence of this field shall imply `false`.
content|May be any valid JSON value/object type.

### Handshakes

The first messages exchanged between the server and the newly connected client shall be of type `$$handshake`.
As of writing, the following shall be communicated/agreed upon during the handshake:
- The respective versions of Buxtehude in use. Each version shall have a minimum supported version, should the versions differ.
- The "team" that the client will join.
- The preferred message format to use.

### Teams

Clients join "teams" when they connect to the server. A message with the destination `name` shall be routed to all clients under this team name, unless
`only_first` is set to true, in which case it shall be routed to the first available team member.

### Availability

Clients may mark themselves as "unavailable" to accept messages of a certain type. What occurs if no clients are available to accept a given message is implementation defined.
