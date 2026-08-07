# pig-pen

A C++ GUI application that visualizes a locally hosted LLM living inside a
10×10 pen. The model is embodied as a blob that perceives and acts only
through tools (`move`, `look`, `eat`) registered with
[scry](https://github.com/crybo-rybo/scry); the GUI shows what the model
observed, decided, and did — live — and logs episodes.

See [DESIGN.md](docs/DESIGN.md) for the full specification, architecture, and
milestones.
