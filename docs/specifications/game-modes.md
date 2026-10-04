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
- `win_by_kills` option exists; its effect is unknown. [?]

## Not specified

Campaign mode (ghosts, rovers, lives, points: values 1200-1320 and `.cam` files), network games.

## Roulette ("goldman" option, off by default)

- With the option on, the winner of a match is remembered; when the next match is set up (after Start Game, before the player list) the bonus-game screen is shown. [S]
- A wheel of six prizes and a ring turn in opposite directions on a circle of 420 positions (6 × value 1004), centre (320,240), radii 200 × 150. Start positions are random, the wheel's speed is 20-39 positions per frame and the ring's is that plus 0-19; the direction is random. [S]
- Enter or Space starts the slow-down: each time a ring passes a slot boundary (every 70 positions) its speed drops by one; below 7 it still moves 6 positions per frame; it stops on a boundary. The ring's boundary passes play the tick sound (1300). [S]
- When both have stopped the prize is the slot under the ring: bomb, flame, kicker, goldflame, skate or clog. Sound 1310 (applause), or 1320 (buzzer) for the clog. Three text lines name the prize. Enter or Space then continues with the match setup; Esc returns to the menu. [S]
- In every round of that next match the previous match's winner (every member of the winning team in team play) starts with one more of that powerup, without the usual cap, and is surrounded by gold sparkles for the first value 1010 (5) seconds of the round. [S]
- The original advances the wheel once per drawn frame with no frame limit; this implementation uses 25 frames per second. [M]
