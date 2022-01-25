TODO
* move vdp_int and coinc bits to VDP status byte, test them there, return that byte and clear on read
* implement fifth sprite check & status register content
* make IO read/write decoding match schematic

No or very minor issues but need to verify another source
* Q-bert (but known to make unknown command 0xC0 to VDP, ignored)
* Pitfall II - Lost Caverns
  * a little chirping during soundtrack - am I not doing silencing correctly?
* Mr. Do!
* Dig Dug
* Roc 'N Rope
* Defender
* Donkey Kong
* Donkey Kong Jr.
* Zaxxon
* Smurf
* Pitfall!
* Looping
* Miner 2049er
  * weird tone at beginning
* Choplifter!
* Burgertime 1983 prototype
* Burgertime
* Spy Hunter Prototype
* Spy Hunter
* Popeye
  * strange high-pitched whistle when there seems like there should be a rest between notes during play
* Super Cross Force
  * there's no animation during explosion - is that normal?
* Beamrider
  * stars show up after dying - is that normal?

Works with minor issues
* Moon Patrol
  * title screen is missing the name 
* Jungle Hunt
  * occasionally aborts with interrupted VDP command

Known serious issues preventing game play
* none

Wontfix
* Frogger
  * repeated note after drawing initial screen, no recognition of game start
* Slither - requires Roller Controller (trackball)

Unknown
* Super Action Controller Test Cartridge (1983) (Nuvatec).rom
