Plugins repo for [shinxbot2](https://github.com/Jayfeather233/shinxbot2).

## Features included

- Stinky Number Argument Validator, from: [itorr](https://github.com/itorr/homo)
- Random Anime Images. Source: [dmoe](https://www.dmoe.cc)
- What's this short for?, from: [itorr](https://github.com/itorr/nbnhhsh)
- Generate fake forwarding messages in group or private chats
- Random Color Palette
- Cyber Cat Raising (co-created with GPT-3.5)
- Cat image for HTTP code. Source: [httpcats](https://httpcats.com/)
- Image OCR (embedded with QQ)
- Image Processing: Symmetry, Rotation, Kaleidoscope
- OpenAI's `gpt-3.5-turbo` API support
- Google's `gemini-pro[-vision]` API support, [url](https://ai.google.dev/docs)
- StableDiffusion's `SDXL-Turbo` API support, [url](https://sdxlturbo.ai/)
- Memes storage
- Most Active User Reminders
- Group member change notifications
- Echo
- Forward messages between different group chats and private chats
- Graylist Management
- Manage multiple bots, with future capability to broadcast messages

## Compile

Run `python generate_cmake.py` under `functions|events`, then run `./make_all.sh`. It will compile plugins into `.so` files under `./lib/functions` and `./lib/events` direcories. Copy them into shinxbot2 repo `./lib/`.

## Develop

Using `class shinxbot` as an example, function parameters are only related to messages, and events accept JSON data. Call methods from functions when receiving chat messages and use methods from events for other prompt messages.

1. Create a new directory in `./src/functions`, place your code there, write a class that inherits from the class in `./src/interfaces/processable.h`, implement the virtual methods, and provide a `create` method to obtain a pointer of type `processable*` (see `functions/getJapaneseImage/Anime_Img.cpp` for reference).
2. If the check() method in the inherited virtual method succeeds, the process() method will be executed.
3. A CMake template is provided in `generate_cmake.py` to compile your functionality, automatically generating the dynamic library in `./lib/functions/`.
4. Start the bot. If the bot is running, you can use the admin account (from `op_list.json`) to send `bot.load [function|event] name` to load it.

Before running the bot, ensure that the dynamic libraries for functions are placed in `./lib/functions | ./lib/events`, or generate them using `generate_cmake.py & make_all.sh`.

When the bot loads a library, it will call `extern "C" processable *create()` to obtain the function pointer. Generally, just returning `new YourFunc()` is sufficient.

When the bot unloads a library, it will call `extern "C" processable *close(processable* p)` to indicate that the library needs to be unloaded. Please properly release all globally `new` obtained pointers, including the parameter `p`.

### Help text convention

`help()` (no args) feeds the aggregated `bot.help` list — it **must be a single brief line** (module name +
one sentence + a pointer to the detailed-help command, e.g.
`"Celeste goldberries query (goldberries.net). Detailed help: gold help"`). Every module's `help()` gets
concatenated back-to-back into one `bot.help` message; a module returning a full usage dump there makes that
message balloon (celeste/hist/openai all did this — fixed).

Detailed usage belongs in the module's own on-demand command (e.g. `gold help` / `gold.help` /
`*nonogram.help`) — any length is fine there, and it can vary by permission via `help(conf, level)`
(`help_level_t`: `public_only` / `group_admin` / `bot_admin`) to show admin-only commands only to admins.
Prefer reusing `resolve_help_level(conf)` from `plugins/lib/help_utils.h` for that permission check (it
matches the framework's own `bot.help` aggregator rule: `bot_admin` requires private chat + op,
`group_admin` requires group chat + group admin) instead of re-deriving it per plugin.
