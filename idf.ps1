# idf.ps1 —— p4-lab 的 idf.py 包装器
# 用法： idf.bat build / idf.bat -p COM3 flash monitor
# 为什么要有这个：每次新开终端环境变量都会丢，这里一次性导好

param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $IdfArgs
)

$idf = "E:\idf\idf5.5.3\v5.5.3\esp-idf"
$py  = "E:\idf\tools\python_env\idf5.5_py3.11_env\Scripts\python.exe"

Set-Location $idf
$env:IDF_PATH = $idf
$env:IDF_TOOLS_PATH = "E:\idf\tools"

& $py "tools\idf_tools.py" export --format key-value | ForEach-Object {
    if ($_ -match '^([A-Za-z_][\w]*)=' + '(.*)$') {
        Set-Item -Path "env:$($Matches[1])" -Value $Matches[2]
    }
}

Set-Location $PSScriptRoot

& $py "$idf\tools\idf.py" @IdfArgs
exit $LASTEXITCODE
