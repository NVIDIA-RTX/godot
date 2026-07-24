# setup-emsdk

This actions step downloads emsdk and installs a version of Emscripten.

## Usage

```yaml
name: "emsdk"
on: [push]

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: mymindstorm/setup-emsdk@v14

      - name: Verify
        run: emcc -v
```

## Cache

To just cache emsdk:

```yaml
- name: Setup emsdk
  uses: mymindstorm/setup-emsdk@v14
  with:
    # Make sure to set a version number!
    version: 1.38.40
    # This is the name of the cache folder.
    # The cache folder will be placed in the build directory,
    #  so make sure it doesn't conflict with anything!
    actions-cache-folder: 'emsdk-cache'

- name: Verify
  run: emcc -v
```

If you want to also cache system libraries generated during build time:

```yaml
env:
  EM_VERSION: 1.39.18
  EM_CACHE_FOLDER: 'emsdk-cache'

jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2
      - name: Setup cache
        id: cache-system-libraries
        uses: actions/cache@v2
        with:
          path: ${{env.EM_CACHE_FOLDER}}
          key: ${{env.EM_VERSION}}-${{ runner.os }}
      - uses: mymindstorm/setup-emsdk@v14
        with:
          version: ${{env.EM_VERSION}}
          actions-cache-folder: ${{env.EM_CACHE_FOLDER}}
      - name: Build library
        run: make -j2
      - name: Run unit tests
        run: make check
```

## Options

```yaml
version:
  description: 'Version to install'
  default: 'latest'
no-install:
  description: "If true will not download any version of Emscripten. emsdk will still be added to PATH."
  default: false
no-cache:
  description: "If true will not cache any downloads with tc.cacheDir."
  default: false
actions-cache-folder:
  description: "Directory to cache emsdk in. This folder will go under $GITHUB_HOME (I.e. build dir) and be cached using @actions/cache."
  default: ''
update:
  description: "Fetch package information for all the new tools and SDK versions"
  default: false
```

See [action.yml](action.yml)
