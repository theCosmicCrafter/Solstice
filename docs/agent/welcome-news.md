# Welcome page News: Krita news and Solstice releases — agent reference

Status: added 2026-10-13 at the user's request. User-facing note:
[docs/versioning.md](../versioning.md), "Updates".

## Behavior

The welcome page's News area is split vertically (`QSplitter`, equal
stretch, neither part collapsible):

- **Krita news** (top): unchanged upstream behavior. The krita.org feeds
  per language (`https://krita.org/<lang>/index.xml`), chosen in the news
  menu's Language section.
- **Solstice releases** (bottom): the Atom feed of the Solstice repository's
  GitHub Releases, `https://github.com/udtren/Solstice/releases.atom`
  (public, no token, HTTP 200 without redirect). While it has no entries, the
  section shows "No Solstice release has been published yet." Clicking an
  entry opens its release page; no analytics parameters are added (they are
  for krita.org links only).

Both feeds are fetched only when the user enables news (`FetchNews` in
kritarc, off by default); the single switch is now "Enable Krita news and
Solstice releases", and its tooltip and the "enable news" text name GitHub.
This keeps the existing privacy rule: no network access unless enabled.

Krita's update checker stays off (`ENABLE_UPDATERS` is OFF; it would announce
Krita releases). The release list is information only: it does not compare
versions or notify.

## Implementation

| Place | Change |
| --- | --- |
| `libs/ui/forms/KisNewsPage.ui` | `newsSplitter` with `kritaNewsSection` (`labelKritaNews`, `listNews`) and `solsticeReleasesSection` (`labelSolsticeReleases`, `labelNoReleases`, `listReleases`) |
| `libs/ui/widgets/KisNewsWidget.{h,cpp}` | Second `MultiFeedRssModel` (`m_releasesModel`) with the release feed, added and removed by `toggleNews()`; both lists share the delegate, kinetic scrolling and hover-clearing event filter; `updateReleasesPlaceholder()` on `feedDataChanged` |
| `libs/ui/KisRssReader.{h,cpp}` | `parseAtomEntry()`: Atom `<entry>` with `title`, `link` (`href` of `rel="alternate"` or no `rel`), `updated` (ISO 8601) as the date and `content` (or `summary`) as the description. RSS `<item>` parsing is unchanged |
| `libs/ui/KisWelcomePageWidget.cpp` | Menu text, tooltip and the "news is off" text |
| `libs/ui/tests/KisMultiFeedRssModelTest.{h,cpp}`, `data/rss_feeds/releases.atom` | `testAtomFeed`: a feed in GitHub's format gives two items with title, link, UTC date, HTML description and the feed title |

`MultiFeedRssModel` shows the first text block of the description, so a
release's first paragraph appears under its title and date.

## Checks

- `KisMultiFeedRssModelTest` (7 tests) passes; run the executable alone (the
  libs-ui suite opens dialogs).
- Manual: with news enabled, the top shows krita.org news and the bottom
  "No Solstice release has been published yet." (until the first GitHub
  Release); after a release is published, it appears with its date and
  opens its page. The splitter resizes both parts. With news disabled,
  nothing is fetched and the "enable news" text is shown.
