# OpenMP
This is the server code for a project I'm working on. It's in pure C. 
It should work, or at least serve as an example, for games requiring state-based real-time network integration. Currently, support for a large volume of players is prioritized over interactivity with the environment.
 
To build the server executable, just enter "make".
Run using the command ./server <port number> <max number of players>

TODO: Plenty of things.
- implement and test timeout/exit packets
- add example client code
- support for Windows
