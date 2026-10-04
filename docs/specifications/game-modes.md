# Game Modes and Match Flow

See `README.md` for tags ([S] code, [D] data, [M] medium, [?] open; "observed" = seen on the original).

## Pre-game

1. **Player list**: ten slots, each OFF, KEY 0, KEY 1, a joystick, or AI. Default: player 1 KEY 0, player 2 AI, others OFF. [observed]
2. **Level and options**: level theme (or random), scheme, wins needed. [observed]
3. At least two active slots are needed to start. [M]

## Free-for-all

- Everyone against everyone. The round ends when at most one contender is left; a dying player counts as a contender for V(25) [20] frames. [S]
- Last survivor wins the round; nobody left is a draw; the clock reaching 0:00 with several alive is a draw. [S][observed]
- Kills: +1 for a kill, −1 for killing yourself (unless disabled by an option). [S]

## Team play

Option `team_play`. [D]

- Two teams; each player's team (0 or 1) comes from the scheme's start lines. [D][S]
- Contenders are counted per team: the round ends when one team has nobody left. [S]
- After the first V(32) [40] frames of a round, players are drawn in their team's colour instead of their own: team 0 in colour 0 (white), team 1 in colour 2 (red). Bombs, flames and the score labels follow. [S, observed for players and labels]
- Computer players do not attack or hunt team-mates. [S]
- Whether team-mates' flames kill each other: they do; the flame check does not look at teams. [S]

## Match

- First to V(310) [2] round wins (option `num_to_win_match`) takes the match. [D]
- Result screens: "DRAW GAME", a results screen after a won round, a victory screen in the winner's colour after the match (team pictures in team play). [observed for draw; pictures present in the data]
- `win_by_kills`: the match goes to the single player with the most kills once that total reaches the target (kills add up over the rounds of the match); round wins then do not end the match, and a suicide does not cost a kill. Not used in team play. [S]

## Not specified

Network games.

## Roulette ("goldman" option, off by default)

- With the option on, the winner of a match is remembered; when the next match is set up (after Start Game, before the player list) the bonus-game screen is shown. [S]
- A wheel of six prizes and a ring turn in opposite directions on a circle of 420 positions (6 × value 1004), centre (320,240), radii 200 × 150. Start positions are random, the wheel's speed is 20-39 positions per frame and the ring's is that plus 0-19; the direction is random. [S]
- Enter or Space starts the slow-down: each time a ring passes a slot boundary (every 70 positions) its speed drops by one; below 7 it still moves 6 positions per frame; it stops on a boundary. The ring's boundary passes play the tick sound (1300). [S]
- When both have stopped the prize is the slot under the ring: bomb, flame, kicker, goldflame, skate or clog. Sound 1310 (applause), or 1320 (buzzer) for the clog. Three text lines name the prize. Enter or Space then continues with the match setup; Esc returns to the menu. [S]
- In every round of that next match the previous match's winner (every member of the winning team in team play) starts with one more of that powerup, without the usual cap, and is surrounded by gold sparkles for the first value 1010 (5) seconds of the round. [S]
- The original advances the wheel once per drawn frame with no frame limit; this implementation uses 25 frames per second. [M]

## Settings

The original keeps these in `options.ini` and edits them on the Options screen (messages 250-263). [S]

| Setting (key) | Choices | Default | Effect |
|---|---|---|---|
| Team Play (`team_play`) | No / Yes | No | Teams from the scheme |
| Random Start (`random_start`) | No / Yes | No | Once per match the ten start positions are shuffled (200 random swaps) |
| Conveyor Speed (`conveyor_speed`) | Low / Medium / High | Medium | Belt speed of conveyor levels |
| Stomped Bombs Detonate (`stomped_bombs_detonate`) | No / Yes | Yes | A bomb under a closing wall explodes, or is removed |
| Win Matches By Kill Total (`win_by_kills`) | No / Yes | No | See above |
| Gold Bomberman (`goldman`) | No / Yes | No | The roulette |
| Enclosement Depth (`enclosement_depth`) | None / A Little / A Lot / All the way! | A Little | How far the closing walls go |
| Play Time (`playtime`) | 1:00, 1:30, 2:00, 2:30, 3:00, 4:00, 5:00, 10:00, Infinite | 2:30 | Round clock |
| Diseases Can Be Destroyed (`diseases_destroyable`) | No / Yes | Yes | Otherwise a destroyed disease reappears elsewhere |
| Disable music during gameplay (`disable_game_music`) | No / Yes | No | |
| Level (`levelno`) | 0-10 or Random Each Game (-1) | 0 | Random picks among levels enabled by values 1150-1160 |

Defaults are those of the tuning values and of an options file written by the original; the original's built-in defaults for a missing file were not all read. [M]
Not carried over: node name, modem and protocol settings, keyboard layout editor, "enhanced memory model", and "Adjust Audio" (a placeholder in the original: message 320).

## Campaign mode

A hidden, visibly unfinished mode of the original, recovered from `campaign.c` and `aliens.c`. All of it is [S] (read in the code, never seen running).

- **Entering**: on the player list of a local game, press C five times; choose one of the `.cam` files (`simple`, `ghosts`, `crouton`); a notice confirms "Campaign Mode Activated!". Computer players are removed from the list and cannot be selected.
- **Campaign file**: one stage per line, `-C,name,level,scheme,rovers,rover speed,ghosts,ghost speed,computer players,difficulty` (difficulty is not used).
- **Stage start**: level and scheme come from the stage; the level screen is skipped and one human player is enough; the stage's number of computer players is seated at random free seats; a notice "Prepare to begin Campaign! (name)".
- **Enemies**: ghosts and rovers are placed on random non-solid cells more than three cells (Manhattan) from every player. A rover clears its cell and neighbours like a player's start and walks blank cells only; a ghost also walks through bricks; neither enters a cell with a bomb. They move like players (speed in 100ths of a pixel per frame). At each cell centre: if the way ahead is blocked they stop and turn left or right at random; otherwise they turn with 1 chance in value 1200 (3). A turn into a blocked direction costs the rest of that update.
- An enemy in a burning cell dies; the flame's owner scores value 1310 (15) for a rover, 1320 (25) for a ghost. Burning a computer player scores value 1300 (250).
- An enemy kills any **human** player whose cell it enters; computer players are not harmed.
- **Lives**: a human player whose death animation has finished returns at the start cell, keeping the inventory, as long as a life is left; each return grants another life unless less than value 101 (60) seconds remain. Computer players do not return.
- **Stage end**: cleared when no enemy has been left for 2 × value 25 frames (2 s), whatever the computer players are doing. Failed when the clock is down to its last second, or when no human player is in play; in the second case the same stage is played again, after "Oh Well! Campaign unsuccessful!". (After a clock failure the original moves on to the next stage.)
- The last-player-standing rule and the closing walls are off (the original reports two contenders at all times).
- After the last stage: "Congratulations! You made it through the whole campaign!" and back to the menu.
- The value 1205 ("chance that the direction change will NOT be towards a human") is not used by the code that was read.
