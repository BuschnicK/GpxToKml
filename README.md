# GpxToKml

Converts a directory of .gpx files to .kml. The primary use-case is taking a [Strava batch download](https://support.strava.com/hc/en-us/articles/216918437-Exporting-your-Data-and-Bulk-Export#h_01GG58HC4F1BGQ9PQZZVANN6WF) and converting all of the files into a format suitable for Google Earth.

# Synopsis
```
C:\development\GpxToKml\bin> .\gpx2kml-Release.exe
Supported options:
  --help                List command line options
  --input_dir arg       Input directory containing GPX files.
  --output_dir arg      Output directory for KML results. Defaults to
                        input_dir.
```
# Results

My Strava tracks from exploring Switzerland by hiking, climbing, skiing, biking.

![Example Imported Strava Activities](img/google_earth.jpeg?raw=true "Strava Tracks in Google Earth")
