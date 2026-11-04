Get-Content D:\Project\Testing\work\scripts\msvc.env | ForEach-Object { $n,$v = $_ -split '=',2; Set-Item "env:$n" $v }
Set-Location D:\Project\Testing
