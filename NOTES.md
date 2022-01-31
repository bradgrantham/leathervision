TODO
* backdrop color not being honored - Super Action Controller should be black on green - https://www.youtube.com/watch?v=FQVKTKH-x2E
* implement backdrop?  Will be visible on NTSC and can match device.  Handling early clock needs care
* Am I correctly handling signed sprite Y?  (Page 2-26 in VDP docs)
* implement fifth sprite check & status register content
* make IO read/write decoding match schematic

| Cartridge | Status | Notes |
| --------- | ------ | ----- |
| Mr. Do! | playable | |
| Dig Dug | playable | |
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
| Q-bert | playable | (but known to make unknown command 0xC0 to VDP, ignored) |
| Pitfall II - Lost Caverns | playable | a little chirping during soundtrack - am I not doing silencing correctly? |
| Miner 2049er | playable | weird tone at beginning |
| Popeye | playable | strange high-pitched whistle when there seems like there should be a rest between notes during play |
| Super Cross Force | playable | there's no animation during explosion - is that normal? |
| Beamrider | playable | stars show up after dying - is that normal? |
| Moon Patrol (prototype) | playable | title screen is missing the name; issue with prototype? |
| Jungle Hunt | playable | |
| Frogger | hang WONTFIX | repeated note after drawing initial screen, no recognition of game start |
| Slither | unknown | appears to work but requires Roller Controller (trackball) |
| Super Action Controller Test Cartridge | playable | but should there be a background graphic? |

