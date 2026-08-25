# Design System

## Why this exists

This project grew three separate web surfaces, built at different times, each
styled independently:

1. A GitHub Pages marketing/docs site (`site/`).
2. A Pi admin console (`pi-server/xteink_print_server/admin_ui/`).
3. An on-device web UI served by the X4 printer's own firmware
   (`firmware/src/ui/WebUiServer.cpp`).

Nothing tied their appearance together, so they drifted — different fonts,
different radii, different border/shadow treatments — even though a user
moving between them (marketing site → pairing a device via the Pi console →
approving a print from their phone against the X4's own hotspot) should read
them as one product.

The GitHub Pages site's stylesheet (`site/assets/style.css`) was chosen as
the canonical source rather than inventing a fourth palette: of the three, it
is the most complete and the most deliberately designed. This document is
that canonical spec, reverse-engineered from the site's existing values plus
two small extensions the Pi console already needed. It exists so future
changes to *any* of the three surfaces have one place to check instead of
three files silently diverging again.

## Color tokens

Light values in `:root`, overridden under `@media (prefers-color-scheme:
dark)`:

```css
:root {
  --bg: #ffffff;
  --fg: #1a1a1a;
  --muted: #5a6270;
  --accent: #2563eb;
  --accent-fg: #ffffff;
  --card-bg: #f6f7f9;
  --border: #e0e2e7;
  --danger: #b3261e;
  --danger-bg: #fdecea;
}

@media (prefers-color-scheme: dark) {
  :root {
    --bg: #14161a;
    --fg: #eef0f3;
    --muted: #9aa2b1;
    --accent: #6ea8fe;
    --accent-fg: #ffffff;
    --card-bg: #1c1f26;
    --border: #2c3038;
    --danger: #ff8478;
    --danger-bg: #3a1a18;
  }
}
```

- `--bg`, `--fg`, `--muted`, `--accent`, `--card-bg`, `--border` are the
  GitHub Pages site's own existing values, unchanged.
- `--accent-fg`, `--danger`, `--danger-bg` are extensions the site doesn't
  need yet but the Pi admin console does (primary-button text, error
  banners). They're kept at the Pi console's pre-existing values since they
  don't conflict with the shared accent and already read well against it.

## Typography

One font stack across all three surfaces, taken verbatim from the GitHub
Pages site:

```css
font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica,
  Arial, sans-serif, "Apple Color Emoji", "Segoe UI Emoji";
```

System fonts only — no webfont or CDN dependency anywhere. This isn't just a
site-level preference: the X4's hotspot mode has zero internet access, so any
surface that pulled a webfont would simply fail to render it. The constraint
applies project-wide.

## Radius scale

| Element | Radius |
|---|---|
| Buttons | 8px |
| Cards / panels | 10px |
| Inline code / pills | 4px |

Applied everywhere — not varied per surface.

## Structural borders

Every card, panel, and header bar uses `1px solid var(--border)`. Never a
heavier weight, never a different color.

## Buttons

**Primary:**

```css
background: var(--accent);
border: 1px solid var(--accent);
color: var(--accent-fg);
border-radius: 8px;
font-weight: 600;
```

**Secondary:**

```css
background: var(--card-bg);
border: 1px solid var(--border);
color: var(--fg);
border-radius: 8px;
font-weight: 600;
```

Secondary hover: `border-color: var(--accent)`.

Padding and font-size may vary by context — a Pi admin-table row action is
compact, an X4 mobile page button is full-width, a site marketing CTA is
large. Color, radius, weight, and border do not vary by context.

## Cards / panels

```css
background: var(--card-bg);
border: 1px solid var(--border);
border-radius: 10px;
box-shadow: 0 1px 2px rgba(0, 0, 0, 0.04);
```

## Header bars

Wherever a surface has a header bar: `border-bottom: 1px solid var(--border)`,
brand/title text `font-weight: 700`, flex row layout — the same visual family
as the site's `.site-header`. This applies even on a surface with no
multi-page nav: the X4 has a single screen, so its header bar just gets the
same border/weight treatment applied to its title, with no nav links to add.

## Implemented by

| Surface | File | Notes |
|---|---|---|
| GitHub Pages site | `site/assets/style.css` | Canonical source of the shared tokens. Currently on the unmerged PR branch `site/pages-shell-and-home`, not yet on the branch this doc lives on. |
| Pi admin console | `pi-server/xteink_print_server/admin_ui/style.css` | |
| X4 on-device web UI | `firmware/src/ui/WebUiServer.cpp` | Inline `<style>` blocks inside `kLoginPageHtml` and `kJobListPageHtml`. |

There is no shared build system tying these three together — different
languages, different deployments, no bundler or imported stylesheet linking
them. This is convention-enforced consistency, not mechanically enforced
consistency. **Any future change to the shared look must update this doc and
all three files together**, or the surfaces will silently drift apart again.
