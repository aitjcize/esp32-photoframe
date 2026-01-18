# Docker Usage

This container downloads `process-cli` from GitHub during build and processes images from a mounted folder.

## Folder layout

Your host folder is mounted to `/data` in the container.

```
/data
├── input
│   ├── image1.jpg
│   ├── image2.png
│   └── AlbumA/
│       ├── photo1.jpg
│       └── photo2.jpg
└── output
```

## Processing behavior

- Images directly under `/data/input` are processed into `/data/output`.
- Images inside subfolders of `/data/input` are processed into matching subfolders under `/data/output`.
- Output folders are created automatically if missing.

## Configure the host path

Set the local folder to mount by editing `.env`:

```
PHOTOFRAME_DIR=/Users/r2kch/Documents/photoframe
```

Expected local structure:

```
/Users/r2kch/Documents/photoframe
├── input
└── output
```

## Build and run

```
docker compose up --build
```

The container exits automatically after processing finishes.


## Copy to SD card

Afterwards, copy the images to the SD card. You will never have converted images faster!