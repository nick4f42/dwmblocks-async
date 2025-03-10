# dwmblocks-async
A [`dwm`](https://dwm.suckless.org) status bar that has a modular, async design, so it is always responsive. Imagine `i3blocks`, but for `dwm`.

![A lean config of dwmblocks-async.](preview.png)

## Features
- [Modular](#modifying-the-blocks)
- Lightweight
- [Suckless](https://suckless.org/philosophy)
- Blocks:
    - [Clickable](#clickable-blocks)
    - Loaded asynchronously
    - [Updates can be externally triggered](#signalling-changes)
    - Specified with program arguments
- Compatible with `i3blocks` scripts

> Additionally, this build of `dwmblocks` is more optimized and fixes the flickering of the status bar when scrolling.

## Why `dwmblocks`?
In `dwm`, you have to set the status bar through an infinite loop, like so:

```sh
while :; do
    xsetroot -name "$(date)"
    sleep 30
done
```

This is inefficient when running multiple commands that need to be updated at different frequencies. For example, to display an unread mail count and a clock in the status bar:

```sh
while :; do
    xsetroot -name "$(mailCount) $(date)"
    sleep 60
done
```

Both are executed at the same rate, which is wasteful. Ideally, the mail counter would be updated every thirty minutes, since there's a limit to the number of requests I can make using Gmail's APIs (as a free user).  

`dwmblocks` allows you to divide the status bar into multiple blocks, each of which can be updated at its own interval. This effectively addresses the previous issue, because the commands in a block are only executed once within that time frame.

## Why `dwmblocks-async`?
The magic of `dwmblocks-async` is in the `async` part. Since vanilla `dwmblocks` executes the commands of each block sequentially, it leads to annoying freezes. In cases where one block takes several seconds to execute, like in the mail and date blocks example from above, the delay is clearly visible. Fire up a new instance of `dwmblocks` and you'll see!

With `dwmblocks-async`, the computer executes each block asynchronously (simultaneously).

## Installation
Clone this repository, modify `config.h` appropriately, then compile the program:

```sh
git clone https://github.com/UtkarshVerma/dwmblocks-async.git
cd dwmblocks-async
vi config.h
sudo make install
```

## Usage
To set `dwmblocks-async` as your status bar, you need to run it as a background process on startup. One way is to add the following to `~/.xinitrc`:

```sh
# The binary of `dwmblocks-async` is named `dwmblocks`
dwmblocks \
    -- 5 0 awk '{print "cpu:" $1}' /proc/loadavg \
    -- 1 0 date '+%a %b %-d %H:%M' &
```

```sh
dwmblocks [options] (-- interval signal program args...)...
```

For each block, the command `program args...` is run every `interval` seconds.
The block can be manually triggered by sending user signal `signal` to the
process.

The general options are:

```
--cmdlength=X
    Maximum possible length of output from block, expressed in number of characters.

--delimiter=X
    The status bar's delimiter that appears in between each block.

--leading-delim
    Adds a leading delimiter to the status bar, useful for powerline.

--clickable
    Enable clickability for blocks. See the "Clickable blocks" section below.
```

### Signalling changes
Most status bars constantly rerun all scripts every few seconds. This is an option here, but a superior choice is to give your block a signal through which you can indicate it to update on relevant event, rather than have it rerun idly.

For example, the volume block has the update signal `5` by default. I run `kill -39 $(pidof dwmblocks)` alongside my volume shortcuts in `dwm` to only update it when relevant. Just add `34` to your signal number! You could also run `pkill -RTMIN+5 dwmblocks`, but it's slower.

To refresh all the blocks, run `kill -10 $(pidof dwmblocks)` or `pkill -SIGUSR1 dwmblocks`.

> All blocks must have different signal numbers!

### Clickable blocks
Like `i3blocks`, this build allows you to build in additional actions into your scripts in response to click events. You can check out [my status bar scripts](https://github.com/UtkarshVerma/dotfiles/tree/main/.local/bin/statusbar) as references for using the `$BLOCK_BUTTON` variable.

To use this feature, pass the `--clickable` argument before any blocks.

Apart from that, you need `dwm` to be patched with [statuscmd](https://dwm.suckless.org/patches/statuscmd/).

## Credits
This work would not have been possible without [Luke's build of dwmblocks](https://github.com/LukeSmithxyz/dwmblocks) and [Daniel Bylinka's statuscmd patch](https://dwm.suckless.org/patches/statuscmd/).
