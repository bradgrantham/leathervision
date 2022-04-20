TODO
* Not doing transparent sprites correctly, I think.  A higher-priority transparent sprite I think covers all sprites under it and shows through the pattern.  May have to implement logic to walk line as outer loop and sprites as inner loop
* Am I correctly handling signed sprite Y?  (Page 2-26 in VDP docs)
* Implement fifth sprite number 
* Implement backdrop outside scan area in NTSC on Rosa?  Will be visible on NTSC and can match device.  Handling early clock needs care
* move sleep_for into platform
* move debug print into platform
* Add dependencies to Makefile or use CMake
* Make IO read/write decoding match schematic
* can debugger be factored out into its own module?
  * add a context template argument with read/write/pc/dump methods?
* can z80state be in main() only?

| Cartridge | Status | Notes |
| --------- | ------ | ----- |
| Mr. Do! | playable | |
| Dig Dug | playable | sprites are incorrect |
| Roc 'N Rope | playable | |
| Defender | playable | |
| Donkey Kong | playable | |
| Donkey Kong Jr. | playable | |
| Zaxxon | playable | |
| Smurf | playable | |
| Pitfall! | playable | |
| Looping | playable | |
| Choplifter! | playable | |
| Burgertime 1983 prototype | playable | |
| Burgertime | playable | |
| Spy Hunter Prototype | playable | |
| Spy Hunter | playable | |
| Moon Patrol (prototype) | playable | should initial rainbow be letters? |
| Beamrider | playable | |
| Super Cross Force | playable | |
| carnival-1982.rom | playable | |
| flapee-byrd-2014.rom | playable | |
| gorf-1983.rom | playable | |
| venture-1982.rom | playable | should beginning block of "?" be something? | 
| star-wars-the-arcade-game-1984.rom | playable | needs trackball |
| cabbage-patch-kids-adventures-in-the-park-1984.rom | playable | lots of unhandled I/O, should sink into into pond? |
| Jungle Hunt | playable | occasional "cmd_phase was reset in ISR".  top of vines should be hidden |
| Q-bert | playable | swearing sound is cut off, unknown command 0xC0 to VDP but seems harmless |
| Pitfall II - Lost Caverns | playable | a little chirping during soundtrack - am I not doing silencing correctly? |
| Miner 2049er | playable | weird tone at beginning |
| Popeye | playable | strange high-pitched whistle when there seems like there should be a rest between notes during play |
| Frogger | unstable | "cmd_phase was reset in ISR" and some video corruption |
| Super Action Controller Test Cartridge | incorrect | is it missing some glyphs because it's assuming Atari joystick for some reason? |
| Slither | unknown | appears to work but requires Roller Controller (trackball) |
| turbo-1982.rom | unplayable | doesn't recognize keypad to select from menu |  
