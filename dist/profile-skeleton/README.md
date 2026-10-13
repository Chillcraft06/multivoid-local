# Profile skeleton

The static half of the one-click mod-manager profile
(`Multivoid-r2modman-profile.zip`). These are the official contents of the
Thunderstore `unreal_shimloader` package (the version in
`shimloader-version.txt`), i.e. UE4SS plus the Lua helper mods that ship
with it, exactly as a manager installs them. Our mod is the declared
dependent; it is NOT stored here, it is assembled per release by
`.github/ci/package-profile.ps1` from the `package.ps1` stage tree.

What is generated per release and therefore absent here:

- `shimloader/mod/Chilltivoid/` (our mod: dlls/main.dll, enabled.txt,
  updater.ps1, manifest.json, docs)
- `shimloader/pak/Chilltivoid/` (skins, from `assets/paks/`)
- `mods.yml` (the manager's package registry, stamped with the release version)

Provenance: third-party files. UE4SS is by the UE4SS team; the Lua mods are
by their respective authors; the set is distributed as the Thunderstore
package `Thunderstore-unreal_shimloader`. They are vendored here so CI can
assemble the full profile without a network fetch.

Refreshing: install the current `unreal_shimloader` package in a manager,
copy everything EXCEPT `shimloader/mod/Chilltivoid/`, `shimloader/pak/`,
and `mods.yml` over this directory, then bump `shimloader-version.txt`.
