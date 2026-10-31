# Fetch GH Release Asset

This action downloads an asset from a GitHub release and provides some release details as output. Private repos are supported.

## Inputs

### `file`

**Required** The name of the file to be downloaded.

### `repo`

The `org/repo` containing the release. Defaults to the current repo.

### `token`

The GitHub token. Defaults to `${{ secrets.GITHUB_TOKEN }}`

### `version`

The release version to fetch from in the form `tags/<tag_name>` or `<release_id>`. Defaults to `latest`.

### `target`

Target file path. Only supports paths to subdirectories of the GitHub Actions workspace directory.

### `regex`

Boolean indicating if `file` is to be interpreted as regular expression. Defaults to `false`.

### `octokitBaseUrl`

The Github API base URL. Useful if you are using Github Enterprise.

## Outputs

### `version`

The version number of the release tag. Can be used to deploy for example to itch.io

### `name`

[Also called a title of a release](https://docs.github.com/en/github/administering-a-repository/managing-releases-in-a-repository). Defaults to the same value as `version` if not specified when creating a release.

### `body`

The body (description) of a release.

## Example usage

Save a single asset as a new file `<workspace>/plague-linux.zip`:

```yaml
uses: dsaltares/fetch-gh-release-asset@master
with:
  repo: 'dsaltares/godot-wild-jam-18'
  version: 'tags/v0.1.18'
  file: 'plague-linux.zip'
  token: ${{ secrets.GITHUB_TOKEN }}
```

Save a single asset as a new file `<workspace>/subdir/plague-linux.zip`:

```yaml
uses: dsaltares/fetch-gh-release-asset@master
with:
  repo: 'dsaltares/godot-wild-jam-18'
  version: 'tags/v0.1.18'
  file: 'plague-linux.zip'
  target: 'subdir/plague-linux.zip'
  token: ${{ secrets.GITHUB_TOKEN }}
```

Save a range of assets as new files in directory `<workspace>`:

```yaml
uses: dsaltares/fetch-gh-release-asset@master
with:
  repo: 'dsaltares/godot-wild-jam-18'
  version: 'tags/v0.1.18'
  regex: true
  file: "plague-.*\\.zip"
  token: ${{ secrets.GITHUB_TOKEN }}
```

Save a range of assets as new files in directory `<workspace>/subdir`:

```yaml
uses: dsaltares/fetch-gh-release-asset@master
with:
  repo: 'dsaltares/godot-wild-jam-18'
  version: 'tags/v0.1.18'
  regex: true
  file: "plague-.*\\.zip"
  target: 'subdir/'
  token: ${{ secrets.GITHUB_TOKEN }}
```
