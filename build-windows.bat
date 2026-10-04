@echo off
python tools\generate_web_assets.py
if errorlevel 1 exit /b 1
idf.py set-target esp32s3
if errorlevel 1 exit /b 1
idf.py build
