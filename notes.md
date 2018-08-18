# Text editor notes

The basic model is a series of independent parts:

- Display (takes user input, displays manipulations, displays text)
- Server (stores the current textual data)
- Filters (syntax highlighting, spell checking, auto-indenter, ...)
- I/O backend (fetcher/serializer)

This is for a "classic" model of a text editor.

Strengths include the modular design and that communication could be
sent over a socket (eg multiple frontends, ...).
Disadvantages include higher latency and complexity, and some of the
interfaces might be quite poor (how would the filters work??).

