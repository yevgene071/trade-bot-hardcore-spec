# ADR 0001: Dashboard Modularization

## Status
Accepted

## Context
The dashboard was previously a single 2000+ line HTML file (`dashboard.html`), which made maintenance, testing, and collaboration difficult. We needed a way to split it into manageable modules while maintaining the requirement of a single-file deployment (bundled into the C++ binary).

## Decision
We decided to decompose the monolith into a modular structure:
1. **HTML Fragments**: Split the UI into reusable fragments in `html/`.
2. **CSS Modules**: Split styles into 17 files in `styles/`, managed via `@import`.
3. **JS Modules**: Split logic into functional directories (`core`, `charts`, `panels`, `app`, `transport`).
4. **Custom Bundler**: Use a lightweight Python script (`inline_dashboard.py`) instead of heavy industry bundlers like Webpack or Esbuild.
   - Support `<!-- include: ... -->` for HTML.
   - Support `@import` for CSS.
   - Support `// @depends-on: ...` for JS topological sorting.
5. **No TypeScript**: Keep plain JavaScript for this iteration to avoid adding a Node.js build dependency to the C++ CI pipeline.
6. **No CDN**: All assets must be local to ensure the dashboard works in air-gapped or restricted network environments.

## Consequences
- **Pros**: Better code organization, easier debugging, clear dependency graph, faster development of new features.
- **Cons**: Custom bundling logic to maintain, manual dependency management via comments.
- **Invariants**: The final output must always be a single `dashboard.bundled.html` file with all assets inlined.
