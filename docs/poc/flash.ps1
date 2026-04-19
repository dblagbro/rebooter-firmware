param(
    [string]$ComPort = "COM3",
    [string]$Firmware = ".\tasmota.bin"
)

Write-Host "Installing esptool if needed..."
py -m pip install esptool

Write-Host "Testing connection..."
py -m esptool --port $ComPort chip_id

Write-Host "Hold button on Sonoff, then press ENTER"
Read-Host

py -m esptool --port $ComPort erase_flash

py -m esptool --port $ComPort --baud 115200 write_flash 0x0 $Firmware

Write-Host "Done."