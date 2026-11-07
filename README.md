# Hospital Emergency Priority System

A C triage decision-support system. Patients are scored against normal vital-sign ranges, emergency alerts override to RED, and a priority queue (RED / ORANGE / YELLOW / GREEN) decides who is called next. Each decision can be explained.

## Build & run
```
gcc -Wall -Wextra -o triage emergency_priority.c
./triage
```

## Features
Register patients, run priority analysis, view queue, call next, search, mark served, report, history log (`history.txt`), persistence (`patients.dat`).
