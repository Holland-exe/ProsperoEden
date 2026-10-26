This alpha keeps each game in a folder of its own with everything that belongs to it, gives cheats their own list, adds a speed limit and a second frame generation method, and fixes a lost graphics device and black boxes that spread over the picture. Performance and compatibility still vary by game.

### New and improved

- **One folder per game.** A game can sit in a folder of its own in `roms/`, together with its update and DLC files, its mods (`mods/`), its cheats (`cheats/`) and its own post-processing presets (`reshade/`). ProsperoEden tells the game from its update and DLC by what each file holds, so the files can have any names. **Delete from console** removes the folder's update and DLC too. Games downloaded from a source now go into a folder of their own the same way. Your files stay where they are and keep working. See [Game files folder](https://github.com/blackbearreloaded/ProsperoEden/blob/main/docs/INSTALL.md#game-files-folder).
- **Cheats in a list of their own.** A game's settings have a **Cheats** row next to **Mods**: every cheat of the game with its own switch, and an **All cheats** switch apart from the Mods one. **Mods** now lists patches and replacement files only. Cheats you chose before stay on.
- **A game's own post-processing presets.** Presets and effects in a game folder's `reshade/` are offered in that game's **Post-processing** list, after the shared ones.
- **Speed limit (new Performance setting).** **Settings > Performance > Speed limit**, and the same row in a game's own settings: 100% (as before), 150%, 200%, 300% or Unlimited. A game locked to 30 FPS can run faster when the console has time to spare; for a game whose logic follows its frame rate, the whole game runs faster.
- **MihawkGen frame generation.** **Frame gen method** in **Settings > Video**, and per game, chooses between MihawkGen, a neural network that comes with the app, and Lossless Scaling with your own copy of it.
- **Update notices (new setting).** **Settings > Diagnostics > Update notices** turns the update dialog at start off. **About** still says when a newer version is on homebrew.page.

### Fixes

- **A lost graphics device while a game loads a new area.** Long runs of uploads and copies could grow past what the console's GPU takes in one submission, and the game stopped. The graphics driver now splits them.
- **Black boxes that grew until they covered the screen.** A few pixels whose colour was not a number spread through a game's glow and blur effects. Such a colour is now written as black, and stays one pixel.
- **Games that ask for a large GPU address space start** instead of stopping at once.
- **No update offered for the version already installed**, when the app's files named an older version than the program.

### Thank you

Thanks to everyone whose contributions landed since v1.004.000:

- [bawsdeep](https://github.com/bawsdeep) — MihawkGen frame generation in the app.
- [mihawk-99](https://github.com/mihawk-99) — MihawkGen itself, the neural frame generation it uses.
- [BlackBearReloaded](https://github.com/blackbearreloaded) — game folders, the Cheats list, a game's own presets, the speed limit, update notices, the graphics fixes, translations and integration.

Thanks also to everyone testing and reporting problems on Discord and GitHub, the **Eden developers** for the emulator, and **Mihawk** and the graphics contributors for the platform work this port builds on.

### Before updating

Back up your saves and settings. The package contains no games, firmware or keys; keep your existing user-provided files. This remains an experimental pre-release.
