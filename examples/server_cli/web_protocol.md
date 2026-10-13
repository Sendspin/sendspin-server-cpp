# server_cli dashboard protocol

The page `server_cli --web` serves talks to it over one WebSocket at `/ws` on the same port, carrying JSON text messages both ways. Image uploads go over HTTP `POST /artwork`. `web_commands.h` and `web_ui.h` implement it.

## Limits and checks

- A browser message is at most 16 KiB (`MAX_COMMAND_BYTES`); a longer one closes the session.
- At most 16 browsers are connected at once, and 64 connections are open; a browser that falls 256 messages behind is closed.
- An HTTP request has 10 s to send its header, then an upload 60 s for its body; one request per connection.
- An upgrade or upload whose `Origin` is not `http://` plus its `Host` is refused with 403, and so is one whose `Host` is not `localhost`, `127.0.0.1` or `[::1]` with the dashboard bound to loopback, or not an IP address, `localhost` or a `.local` name with it bound beyond loopback (DNS rebinding). A request without `Origin` (not a browser) is allowed.
- Every text message the server sends is valid UTF-8: a byte from the network that is not is replaced by U+FFFD.

## Server to browser

Every message has a `type`.

- `snapshot`, first on connect: `server` (`name`, `server_id`, `listen_port`, `loopback_only`, `max_image_bytes`), `timing_interval_s`, `timing` (the timing points kept, oldest first) and `log` (the last 500 log lines). A `state` follows.
- `state`, after every listener event, after every command and once a second: `one_group`, `one_group_fixed` (set by `--one-group`), `loop`, `live` (`--stdin` given), `media` (`--media-dir` given), `clients` and `groups`.
  - A client: `client_id` (empty for one found by browsing and not reached yet), `name`, `url`, `address`, `connected`, `accepted`, `discovered`, `reconnecting`, `approved`, `unpaired_access`, `paired`, `pairing`, `credential_mismatch`, `offers` (`psk`, `static`, `digits`, `qr`), `roles` (the role versions it may be activated with, `ClientInfo::roles`), `group` (its group's id, or null), `format` (its player stream's `codec`, `sample_rate`, `channels` and `bit_depth`, or null) and, once its `client/state` reported them, `volume` and `muted`.
  - A group: `id`, `name`, `server_group`, `playing`, `members` (client ids) and `playback`, null when nothing plays: `live`, `phase`, `paused`, `latency_ms`, then for a file `title`, `file`, `position_s` and `duration_s`, for a routed source `source` (its client id), or for standard input `written`, `dropped` and `ended`.
- `log`: `line`, one line server_cli printed.
- `timing`: `point`, a timing point: `t` (Unix milliseconds) and `clients`, by client id, each `n` (time replies in the interval), `p50_us`, `p99_us` and `max_us` (their hold, as server_cli's `timing` reports it).
- `pairing`: `event`, `client_id` and `name`; `pending` with the device's `message`, `code_needed` with `method` (`psk`, `static` or `dynamic`) and `format` (`token`, `digits` or `qr`), or `result` with `result` (`paired`, `cancelled`, `code_mismatch`, `token_mismatch`, `method_not_supported`, `client_timed_out`, `client_cancelled`, `concurrent_attempt`, `timed_out`, `persistence_failed`, `protocol_error`, `connection_lost` or `server_stopped`).
- `result`, the answer to a command: `id` (null when the command had none), `ok`, and `error` or, for some commands, `data`.

## Browser to server

A command is an object with `id` (an integer from 0 to 4294967295) and `cmd`; a group is its `id` and a client its `client_id`.

| `cmd` | Fields | Does |
| --- | --- | --- |
| `connect` | `url` | Dials a client at a `ws://` URL |
| `approve`, `revoke` | `client`, and for `approve` optionally `source` (true to cover the source role too) | Approves a client for playback, or revokes that |
| `route` | `client`, `group` | Routes the client's source into the group, as the REPL's `route` |
| `pair` | `client`, `method` (`psk`, `static`, `digits`, `qr`, or absent for the code the client shows) | Starts a pairing |
| `code`, `token` | `client`, `code` or `token` | Enters the code or pairing token a pairing asks for |
| `cancel_pairing`, `unpair` | `client` | Cancels a pairing, or removes one |
| `create_group` | `name`, `client` | Creates a group with the client in it; `data.group` is its id |
| `rename_group` | `group`, `name` | Renames a group |
| `move` | `client`, `group` | Moves a client into a group |
| `format` | `client`, `format` (an object of `codec`, `sample_rate`, `channels`, `bit_depth`, or null) | Sets or clears the client's player format |
| `volume`, `mute` | `group`, `volume` (0 to 100) or `muted` | Sets the group's volume or mute |
| `list` | `path` | Lists a directory of the media directory, directories first, by name: `data.entries` (`name`, `directory`), at most 2,000, and `data.truncated` |
| `play`, `queue` | `group`, `path` | Plays or queues a file of the media directory |
| `pause`, `resume`, `stop`, `next` | `group` | Transport, as the REPL's commands; `next` skips to the file after the one playing (below) |
| `seek` | `group`, `seconds` | Seeks the file being written |
| `loop` | `group`, `on` | Loops files, as `loop on` |
| `live` | `group` | Plays standard input (`--stdin`) to the group |
| `one_group` | `on` | Keeps every client in the server's group, as `--one-group` |
| `artwork` | `group`, `source` (`album` or `artist`) | Clears the group's image |
| `colors` | `group`, `colors` (names as `colors` takes them, each `rrggbb`; empty clears) | Sets the group's colors |

`pause`, `resume`, `seek` and `next` answer `ok:false` when they find nothing to act on, such as nothing playing; the log line says more. `next` seeks the file being written to its start when it is the file after the one playing, else to its end, where the feeder goes on as at the end of any file: the last file without loop ends playback, with loop the first file plays again, and a file shorter than the feeder writes ahead may be skipped with the one playing. On standard input it answers that it cannot seek.

A `path` is relative to the media directory, with `/` between its parts; a path that leads outside it, by `..` or a symbolic link, is refused.

## Image upload

`POST /artwork?group=<id>&source=<album|artist>` with the JPEG or PNG file as the body and the header `X-Sendspin-Dashboard: 1` sets the group's image. The origin, the header, the target (400) and the group (404) are checked from the request's header, before its body is read, and a Content-Length past 32 MiB (`artwork_limits::MAX_IMAGE_BYTES`) is refused with 413. The answer is JSON, `{"ok":true}` or `{"ok":false,"error":...}`.
