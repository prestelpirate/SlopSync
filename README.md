<<<<<<< HEAD
# SlopSync
j-pilot plugin: Two-way sync of J-Pilot's **calendar**, **contacts** and **to-dos** with a Radicale CalDAV/CardDAV server.
=======
# Radicale Sync – J-Pilot plugin

Two-way sync of J-Pilot's **calendar**, **contacts** and **to-dos** with a
[Radicale](https://radicale.org) CalDAV/CardDAV server.

## Build and install

Needs: `libgtk-3-dev`, `libical-dev`, `libpisock-dev`, the `curl` program and
the J-Pilot 2.1.0 source tree (for `libplugin.h`; default
`~/Downloads/jpilot-2.1.0`, override with `make JPILOT_SRC=...`).

    make                 # builds libradicalesync.so
    make install         # copies it to ~/.jpilot/plugins/

Restart J-Pilot. The plugin then appears under **Plugins → Radicale Sync**.

## Setup

1. Open **Plugins → Radicale Sync**, then click **Settings…**.
2. Enter the Radicale address (for example `http://server:5232/`), your username and password.
3. Click **Find collections**. Then choose the calendar, the to-do calendar and the address book.
   The calendar and the to-dos can share one collection.
4. Choose what happens when an item changed on both sides. Optionally enable
   **Sync with Radicale automatically during each HotSync**.
5. Click **Save**, then **Sync now**.

## How it syncs

* The first sync links items that exist on both sides by matching:
  * events by description and start time,
  * to-dos by description,
  * contacts by name and company.

  Everything else is copied across. Existing data is not duplicated.
* After that, an item changed on one side is copied to the other. Deletions are copied too.
  When an item changed on both sides, the conflict setting decides which version to keep.
* Server properties that the Palm cannot hold are kept when J-Pilot sends an update. Examples
  are photos, extra phone numbers, attendees, and modified single occurrences.
* With HotSync syncing enabled, server changes are pulled in *before* the Palm
  connects, and Palm changes are sent *after* the HotSync.
* Safety: if one side is suddenly empty but many items were synced before, the
  plugin refuses to delete anything and logs a message instead.

## Files

`~/.jpilot/radicalesync/`

* `radicalesync.conf` stores the settings. It includes the password, so the file has mode 0600.
* `*.state` files record which J-Pilot record belongs to which server object.
* `*.state.d/` holds the last copy of each server object, used for merging.

To force a fresh first sync of a collection, delete its `.state` file and `.state.d` folder.

## SQLite storage

J-Pilot 2.1.0 can keep its data in SQLite (`jpilot -S`). The plugin API only
reads and writes the standard `.pdb`/`.pc3` files, so the plugin refuses to
sync in that mode and says so in the log.

## Limits of the Palm format

* Events can't span midnight. A timed event that ends on a later day is cut at 23:59.
  A multi-day all-day event becomes a daily repeat.
* Repeat rules the Palm can't express (hourly, several weekdays per month …)
  are imported as a single occurrence.
* A contact holds up to 7 phone numbers or e-mail addresses, 3 addresses and 2 IM handles.
  Extra values are kept on the server but not shown in J-Pilot.
* Leading and trailing spaces in notes are trimmed by libical.

## Tests

`test/rstest` runs the sync engine against a live Radicale. It uses in-memory stand-ins
for J-Pilot's database functions and is seeded from real `.pdb` files:

    make test/rstest
    JPILOT_HOME=/tmp/rstest-home ./test/rstest http://127.0.0.1:5232/ USER PASS DIR_WITH_PDBS

The server must have empty collections `/USER/calendar/` (events), `/USER/tasks/`
(to-dos) and `/USER/contacts/` (address book).
>>>>>>> 15f0d10 (Radicale Sync plugin for J-Pilot: two-way CalDAV/CardDAV sync)
