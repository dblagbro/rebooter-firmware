param(
    [string]$Port = "COM10",
    [string]$Output = "C:\dev\rebooter-firmware\docs\backup-sonoff-s31-full-4mb.bin",
    [string]$SizeHex = "0x400000",
    [string[]]$BaudRates = @("74880", "57600", "38400"),
    [int]$AttemptsPerBaud = 3
)

$ErrorActionPreference = "Stop"

function Write-Step($message) {
    Write-Host ""
    Write-Host "== $message ==" -ForegroundColor Cyan
}

function Get-EsptoolCommand($port, $baud, $sizeHex, $output) {
    return @(
        "py", "-m", "esptool",
        "--port", $port,
        "--baud", $baud,
        "read-flash", "0", $sizeHex, $output
    )
}

function Invoke-Esptool($arguments) {
    $outputLines = New-Object System.Collections.Generic.List[string]
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $arguments[0]
    $argList = @()
    for ($i = 1; $i -lt $arguments.Count; $i++) {
        $arg = [string]$arguments[$i]
        if ($arg -match '\s') {
            $argList += '"' + ($arg -replace '"', '\"') + '"'
        } else {
            $argList += $arg
        }
    }
    $psi.Arguments = ($argList -join ' ')
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $false

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $psi
    [void]$process.Start()

    while (-not $process.HasExited) {
        while (-not $process.StandardOutput.EndOfStream) {
            $line = $process.StandardOutput.ReadLine()
            $outputLines.Add($line)
            Write-Host $line
        }
        while (-not $process.StandardError.EndOfStream) {
            $line = $process.StandardError.ReadLine()
            $outputLines.Add($line)
            Write-Host $line
        }
        Start-Sleep -Milliseconds 100
    }

    while (-not $process.StandardOutput.EndOfStream) {
        $line = $process.StandardOutput.ReadLine()
        $outputLines.Add($line)
        Write-Host $line
    }
    while (-not $process.StandardError.EndOfStream) {
        $line = $process.StandardError.ReadLine()
        $outputLines.Add($line)
        Write-Host $line
    }

    return @{
        ExitCode = $process.ExitCode
        Output = $outputLines
    }
}

Write-Step "Flash Backup Helper"
Write-Host "Port: $Port"
Write-Host "Output: $Output"
Write-Host "Size: $SizeHex"
Write-Host "Baud sequence: $($BaudRates -join ', ')"
Write-Host "Attempts per baud: $AttemptsPerBaud"
Write-Host ""
Write-Host "Notes:" -ForegroundColor Yellow
Write-Host "- Full 4 MB flash is 0x400000"
Write-Host "- 0x40000 is only 256 KB"
Write-Host "- Each failed attempt restarts from byte 0; esptool does not resume mid-file"

foreach ($baud in $BaudRates) {
    for ($attempt = 1; $attempt -le $AttemptsPerBaud; $attempt++) {
        Write-Step "Baud $baud, attempt $attempt of $AttemptsPerBaud"
        Write-Host "1. Unplug power from the Sonoff."
        Write-Host "2. Hold GPIO0/button low."
        Write-Host "3. Apply power while still holding."
        Write-Host "4. Press Enter now to start the read."
        Read-Host | Out-Null

        $cmd = Get-EsptoolCommand -port $Port -baud $baud -sizeHex $SizeHex -output $Output
        Write-Host ""
        Write-Host "Starting esptool..." -ForegroundColor Green
        Write-Host "Release GPIO0 once the read has clearly started." -ForegroundColor Green

        $result = Invoke-Esptool -arguments $cmd
        if ($result.ExitCode -eq 0 -and (Test-Path $Output)) {
            $size = (Get-Item $Output).Length
            Write-Host ""
            Write-Host "SUCCESS: backup completed." -ForegroundColor Green
            Write-Host "Saved: $Output"
            Write-Host "Bytes: $size"
            exit 0
        }

        Write-Host ""
        Write-Host "Attempt failed." -ForegroundColor Yellow
        if (Test-Path $Output) {
            try {
                Remove-Item -LiteralPath $Output -Force
            } catch {
                Write-Host "Could not remove partial output file: $Output" -ForegroundColor Yellow
            }
        }
    }
}

Write-Host ""
Write-Host "All attempts failed." -ForegroundColor Red
exit 1
