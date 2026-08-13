# Wintergreen

A highly focused lightweight e-book reader for the Xteink X4, whose goal is to optimize the reading experience and be as usable as possible

A fork of [Nous](https://github.com/unitreign/nous), which is a fork of [Microreader](https://github.com/CidVonHighwind/wintergreen)

Some goals:

- Syncs to a scripted NAS server
  - Because all books have to be converted to a binary format, the NAS server runs a script to convert all of them locally so that the Xteink can pull just the binary files and not the unconverted epubs. This saves space and makes syncing *much* faster.
  - All of the book reading progress is saved to a simple json file! And when a book is finished, it gets deleted and saved to a different json file listing all your finshed books!
- Bluetooth clicker support with auto pair (only supports reprogammable clickers with left/right arrow for page navigation)
- Nix-first building!!
- No unnecessary UI, stat tracking or other extra features. It's meant for very fast and productive reading, and nothing else to distract you from that. This is about as lightweight as you'd get!!

DISCLAIMER: You should not flash this on an locked device!! 
