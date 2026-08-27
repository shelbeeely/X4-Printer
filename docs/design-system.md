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

This doc previously specified a blue, system-font palette lifted from the
GitHub Pages site. It's superseded here by a retro-70s orange design system —
mustard/rust/brown tones, a display + body font pairing, pill-shaped buttons,
and a small set of CSS-only entrance/hover animations — rolled out to all
three surfaces at once so none of them drift from the others. There is still
one canonical spec (this doc); only its content changed.

## Color tokens

Light values in `:root`, overridden under `@media (prefers-color-scheme:
dark)`:

```css
:root {
  --bg: #f7ecd1;
  --card-bg: #fdf6e3;
  --border: #e0c99a;
  --fg: #3a2115;
  --muted: #8a6f4e;
  --accent: #e17a25;
  --accent-fg: #2a1608;
  --gold: #e19d25;
  --accent2: #bd361e;
  --sage: #6b7048;
  --danger: #bd361e;
  --danger-bg: #f6ded6;
}

@media (prefers-color-scheme: dark) {
  :root {
    --bg: #1c1109;
    --card-bg: #3a2314;
    --border: #5c3a1f;
    --fg: #f3e3c4;
    --muted: #b89878;
    --accent: #e17a25;
    --accent-fg: #2a1608;
    --gold: #e19d25;
    --accent2: #bd361e;
    --sage: #8f9463;
    --danger: #ff6a47;
    --danger-bg: #3a1810;
  }
}
```

`--accent` (mustard-orange) is the interactive/primary color; `--gold` is a
secondary accent used for stat values and step numbers — not for primary
buttons. `--accent2` and `--sage` (a brick-red and a muted green) are
reserved tertiary accents: declared in every surface's token set for
parity, but not yet consumed via `var()` anywhere. The header divider's
second wave is visually the same brick-red as `--accent2`, but it's a
hardcoded hex inside an inline SVG data URI (custom properties can't be
referenced from a data: URI), so keep that hex in sync with `--accent2` by
hand if either changes. `--danger`/`--danger-bg` cover error banners and
the X4's wrong-PIN page; note the dark-mode `--danger` is a brighter
`#ff6a47` rather than the light-mode `#bd361e` — reusing the light value in
dark mode reads at under 3:1 contrast against the dark card/background
tones, well short of WCAG AA.

## Typography

Two-face pairing, loaded via Google Fonts:

```css
@import url(https://fonts.googleapis.com/css2?family=Boogaloo&family=Nunito+Sans:wght@400;600;700&display=swap);
```

- **"Boogaloo"** — display face, used only on brand marks and headings
  (`.brand`, `h1`–`h4`). Never for body copy or UI controls; it's a bold
  condensed display face, not a text face.
- **"Nunito Sans"** — everything else (body copy, buttons, form labels,
  table text). Falls back to the previous system-font stack
  (`-apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial,
  sans-serif, "Apple Color Emoji", "Segoe UI Emoji"`) if the webfont hasn't
  loaded.

This is a deliberate change from the previous system-fonts-only rule. The
X4's hotspot mode still has zero internet access, so the `@import`/`<link>`
simply fails there — both pages fall back to the system stack, which is
still legible and still on-brand-adjacent (the fallback stack is the same
one the old blue system used). Nothing on the X4 depends on the webfont
loading successfully; it's a progressive enhancement available whenever the
device is in station mode (joined to a real, internet-connected network).

## Radius scale

| Element | Radius |
|---|---|
| Buttons | 999px (pill) |
| Cards / panels | 16px |
| Inline code / small controls (inputs, fieldsets) | 6–14px, context-dependent |

Buttons are always fully pill-shaped regardless of surface. Cards/panels are
always 16px. Smaller structural elements (text inputs, `<code>`, fieldsets)
use a proportionally smaller radius in the same rounded family rather than a
fixed value — see each surface's stylesheet for its exact per-element radii.

## Structural borders

Every card, panel, and header-adjacent divider uses `1px solid
var(--border)`. Never a heavier weight, never a different color.

## Buttons

**Primary:**

```css
background: var(--accent);
border: 1px solid var(--accent);
color: var(--accent-fg);
border-radius: 999px;
font-weight: 600;
animation: ctaGlow 2.5s ease-in-out infinite;
```

**Secondary:**

```css
background: var(--card-bg);
border: 1px solid var(--border);
color: var(--fg);
border-radius: 999px;
font-weight: 600;
transition: border-color .15s ease, transform .1s ease;
```

Secondary hover: `border-color: var(--accent)`. Both primary and secondary
buttons scale down slightly on press (`transform: scale(.96–.97)` on
`:active`) for tactile feedback.

Padding and font-size may vary by context — a Pi admin-table row action is
compact, an X4 mobile page button is full-width, a site marketing CTA is
large. Color, radius, weight, and border do not vary by context.

## Cards / panels

```css
background: var(--card-bg);
border: 1px solid var(--border);
border-radius: 16px;
box-shadow: 0 1px 2px rgba(58, 33, 21, 0.08);
```

## Header bars and the wavy divider

Every surface's header bar gets a two-tone wavy SVG divider along its bottom
edge instead of a plain border — a repeating background-image data URI
(gold wave layered over a translucent rust wave), animated with a continuous
`waveScroll` drift that reads as an infinite scroll rather than a back-and-
forth bob: the keyframe moves `background-position-x` by exactly one tile
width (40px, matching the SVG tile's own width) with a `linear` timing
function, so the pattern wraps seamlessly with no visible reset:

```css
@keyframes waveScroll {
  from { background-position-x: 0; }
  to { background-position-x: 40px; }
}
```

Brand/title text uses the Boogaloo display face (see Typography above)
rather than a bare weight bump. This applies even on a surface with no
multi-page nav: the X4 has a single screen, so its header just gets the same
wave-divider + display-font title treatment, with no nav links to add.

## Motion

Three shared keyframe animations, CSS-only (no animation library — see
`docs/design-system.md`'s reasoning below and `tools/xtc-wasm/README.md`
for why the X4's *other* enhancement, client-side document preview, needed
a real library/WASM but this one doesn't):

- **`fadeInUp`** — entrance animation for cards, stat tiles, and page
  sections (`opacity`/`translateY(10–14px)` → resting state), staggered by
  ~0.05–0.06s per sibling where multiple cards enter together (stat grids,
  card grids).
- **`waveScroll`** — the header divider's continuous, one-directional drift, above.
- **`ctaGlow`** — a soft pulsing `box-shadow` on primary buttons only.

CSS animations were sufficient for this goal (no library needed); the X4's
other WASM work is a genuinely separate problem (decoding binary print data),
described in `tools/xtc-wasm/README.md`.

## Implemented by

| Surface | File | Notes |
|---|---|---|
| GitHub Pages site | `site/assets/style.css` | Canonical source of the shared tokens (font `@import` lives here; all four site pages share this one file). |
| Pi admin console | `pi-server/xteink_print_server/admin_ui/style.css` | Font `@import` at the top of the stylesheet, same as the site. |
| X4 on-device web UI | `firmware/src/ui/WebUiServer.cpp` | Inline `<style>` blocks inside `kLoginPageHtml` and `kJobListPageHtml`, each with a non-blocking font `<link>` pair (`rel="preload"` + a `media="print"`/`onload` swap, so a stalled fetch in hotspot mode can't hold up first paint) in place of the `@import` the other two surfaces use. The wrong-PIN error page in `handleLogin()` is a bare `<style>`/`<p>` fragment with no `<head>` of its own — it has no font `<link>` and simply renders in the fallback stack. |

There is no shared build system tying these three together — different
languages, different deployments, no bundler or imported stylesheet linking
them. This is convention-enforced consistency, not mechanically enforced
consistency. **Any future change to the shared look must update this doc and
all three files together**, or the surfaces will silently drift apart again.
