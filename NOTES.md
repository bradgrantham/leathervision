controller change automation
    #ifdef ENABLE_AUTOMATION
    need clk_t& in ColecoHW
    record
        --record-controllers filename.txt
        open in main, pass lambda to ColecoHW to write to file
            maybe lambda has &clk
            "clock# {k,j} # setbits resetbits"
        in ColecoHW::io_read, if state has changed, call lambda
            so need to store previous state all the time
    playback
        --playback-controllers filename.txt
        read all in on init into deque of struct ControllerEvent { clk_t clk; bool JoystickNotKeypad; enum Controller which; uint8_t setbits; uint8_t resetbits; }
            pass deque to ColecoHW?  Or pass some kind of GetNextMatchingEvent lambda?
        in ColecoHW::io_read, if clock is > controller_events.clk and which == read address, assert in same state as JoystickNotKeypad, set and reset bits and return current value

TODO
* Add dependencies to Makefile or use CMake
* Am I correctly handling signed sprite Y?  (Page 2-26 in VDP docs)
* Implement fifth sprite number 
* Make IO read/write decoding match schematic
* Implement backdrop outside scan area in NTSC on Rosa?  Will be visible on NTSC and can match device.  Handling early clock needs care

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
| Moon Patrol (prototype) | playable | |
| Beamrider | playable | |
| Super Cross Force | playable | |
| Jungle Hunt | unstable | occasional "cmd_phase was reset in ISR".  top of vines should be hidden |
| Q-bert | playable | (but known to make unknown command 0xC0 to VDP, ignored) |
| Pitfall II - Lost Caverns | playable | a little chirping during soundtrack - am I not doing silencing correctly? |
| Miner 2049er | playable | weird tone at beginning |
| Popeye | playable | strange high-pitched whistle when there seems like there should be a rest between notes during play |
| Super Action Controller Test Cartridge | playable | is it missing some glyphs because it's assuming Atari joystick for some reason? |
| Frogger | hang WONTFIX | repeated note after drawing initial screen, no recognition of game start |
| Slither | unknown | appears to work but requires Roller Controller (trackball) |

