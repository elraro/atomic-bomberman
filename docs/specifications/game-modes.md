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
- After the first V(32) [40] frames of a round, players are drawn in their team's colour instead of their own: team 0 in colour 0 (white), team 1 in colour 2 (red). Bombs and flames follow. [S]
- Computer players do not attack or hunt team-mates. [S]
- Whether team-mates' flames kill each other: they do; the flame check does not look at teams. [S]

## Match

- First to V(310) [2] round wins (option `num_to_win_match`) takes the match. [D]
- Result screens: "DRAW GAME", a results screen after a won round, a victory screen in the winner's colour after the match (team pictures in team play). [observed for draw; pictures present in the data]
- `win_by_kills` option exists; its effect is unknown. [?]

## Not specified

Campaign mode (ghosts, rovers, lives, points: values 1200-1320 and `.cam` files), the "goldman"/roulette feature between rounds, network games.
