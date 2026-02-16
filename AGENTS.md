# Flathub Update Strategy

Use this checklist for every Flathub update:

1. Implement and commit code changes in this repository first.
2. Bump patch/minor/major version in `meson.build` according to change scope.
3. Update release notes in both:
   - `data/dev.rotstein.SmashedPumpkin.metainfo.xml.in` (`<releases>`)
   - `src/app.c` (`adw_about_dialog_set_release_notes_version` and notes text)
4. Commit the release metadata changes.
5. Update `.flathub/dev.rotstein.SmashedPumpkin/dev.rotstein.SmashedPumpkin.json`:
   - Set `modules[0].sources[0].commit` to the exact app release commit hash from step 4.
6. Commit the Flathub manifest change separately.
7. Push commits, then update/create the Flathub PR with the manifest commit.

Versioning policy:
- Patch (`x.y.Z`) for bugfixes/small improvements.
- Minor (`x.Y.0`) for new user-facing features.
- Major (`X.0.0`) for breaking or broad behavior changes.
