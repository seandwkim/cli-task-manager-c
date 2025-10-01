# CLI Task Manager (C99)

A lightweight **command-line task manager** written in C.  
Supports due dates, persistent storage, deletion history, HTTP server view, and a background reminder watcher.

---

## Highlights
- Add, list, and delete tasks with optional due dates/times  
- Persistent storage in `tasks.txt` and `removed.txt`  
- Human-friendly parsing: `today`, `tomorrow`, `MM/DD`, `HH:MM`, `AM/PM`  
- HTTP server to view tasks in plain text or JSON (`serve <port>`)  
- Reminder watcher (`watch`) to notify before deadlines  
- Pure C99 implementation — no external libraries  

---

## Build

```bash
make
# produces ./task_manager
```

Clean:
```bash
make clean
```

---

## Usage

Show help:
```bash
./task_manager help
```

Add tasks:
```bash
./task_manager add "finish report" today 5pm
./task_manager add "buy groceries" 10/05
```

List tasks:
```bash
./task_manager list
```

Delete by ID:
```bash
./task_manager delete 2
./task_manager removed
```

HTTP server:
```bash
./task_manager serve 8080
# open http://127.0.0.1:8080
# JSON: http://127.0.0.1:8080/json
```

Reminder watcher:
```bash
./task_manager watch 60 10
# scans every 60s, notifies 10min before due
```

---

## Environment Variables

- `CLITASK_FILE` — active tasks file (default: `tasks.txt`)  
- `CLITASK_REMOVED` — removed tasks file (default: `removed.txt`)  
- `CLITASK_ALL_LIMIT` — limit in “All Tasks” list (default: 20)  
- `USE_COLOR=0` — disable ANSI colors  

---

## Notes

- Runs on macOS and Linux with `gcc` (C99 standard).  
- Tasks and removed tasks are stored as plain text files.  
- Designed to be simple, portable, and hackable.  
