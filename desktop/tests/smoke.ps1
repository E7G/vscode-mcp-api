param(
    [string]$Executable = (Join-Path $PSScriptRoot '..\build-check-vs2026\Release\vscode-mcp.exe'),
    [int]$Port = 39741
)
$ErrorActionPreference = 'Stop'
$workspace = Join-Path $env:TEMP ('mcp-qt-tests-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $workspace | Out-Null
[IO.File]::WriteAllText((Join-Path $workspace 'sample.txt'), ("alpha beta" + [Environment]::NewLine + "second line"), [Text.UTF8Encoding]::new($false))
function Write-TestArchive([string]$filename, [hashtable]$entries) {
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::Open((Join-Path $workspace $filename), [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($name in $entries.Keys) {
            $item = $archive.CreateEntry($name)
            $writer = [IO.StreamWriter]::new($item.Open(), [Text.UTF8Encoding]::new($false))
            try { $writer.Write($entries[$name]) } finally { $writer.Dispose() }
        }
    } finally { $archive.Dispose() }
}
Write-TestArchive 'sample.docx' @{
    'word/document.xml' = '<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body><w:p><w:r><w:t>Hello DOCX</w:t></w:r></w:p></w:body></w:document>'
}
Write-TestArchive 'sample.xlsx' @{
    'xl/sharedStrings.xml' = '<sst xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><si><t>Hello XLSX</t></si></sst>'
    'xl/worksheets/sheet1.xml' = '<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><sheetData><row r="1"><c r="A1" t="s"><v>0</v></c></row></sheetData></worksheet>'
}
New-Item -ItemType Directory (Join-Path $workspace 'sub') | Out-Null
[IO.File]::WriteAllText((Join-Path $workspace 'sub\match.txt'), 'needle', [Text.UTF8Encoding]::new($false))
$originalPath = $env:PATH
$previousToken = $env:VSCODE_MCP_AUTH_TOKEN
$env:PATH = 'C:\Qt\6.8.3\msvc2022_64\bin;' + $env:PATH
$env:VSCODE_MCP_AUTH_TOKEN = 'smoke-test-token'
$proc = $null
$passed = 0
function Assert($truth, [string]$label) {
    if (-not $truth) { throw "FAIL: $label" }
    $script:passed++
    Write-Output "PASS: $label"
}
function Rpc([string]$name, [hashtable]$arguments = @{}) {
    $request = @{jsonrpc = '2.0'; id = $script:passed + 1; method = 'tools/call'; params = @{name = $name; arguments = $arguments}}
    $json = $request | ConvertTo-Json -Depth 12 -Compress
    $response = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/mcp" -Method Post -Headers @{Authorization = 'Bearer smoke-test-token'} -ContentType 'application/json' -Body ([Text.Encoding]::UTF8.GetBytes($json)) -TimeoutSec 8
    $envelope = $response.result
    $payload = $envelope.content[0].text
    $data = $null
    try { $data = $payload | ConvertFrom-Json } catch { $data = $payload }
    return @{error = [bool]$envelope.isError; data = $data}
}
try {
    $exe = [IO.Path]::GetFullPath($Executable)
    if (!(Test-Path $exe)) { throw "Missing executable: $exe" }
    $proc = Start-Process -FilePath $exe -ArgumentList @('--headless', '--workspace', ('"' + $workspace + '"'), '--port', $Port) -PassThru -WindowStyle Hidden
    $ready = $false
    for($i=0; $i -lt 40; $i++) {
        Start-Sleep -Milliseconds 250
        try {
            $health = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/health" -TimeoutSec 1
            if ($health.status -eq 'ok') { $ready = $true; break }
        } catch { }
        if ($proc.HasExited) { throw "Server exited early: $($proc.ExitCode)" }
    }
    Assert $ready 'headless MCP health'
    try {
        Invoke-RestMethod -Uri "http://127.0.0.1:$Port/mcp" -Method Post -ContentType 'application/json' -Body '{"jsonrpc":"2.0","id":1,"method":"ping"}' -ErrorAction Stop | Out-Null
        throw 'anonymous access unexpectedly succeeded'
    } catch {
        Assert ($_.Exception.Message -match '401|Unauthorized') 'Bearer authentication'
    }
    $list = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/mcp" -Method Post -Headers @{Authorization = 'Bearer smoke-test-token'} -ContentType 'application/json' -Body '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'
    Assert (($list.result.tools.name -contains 'edit_block') -and ($list.result.tools.name -contains 'search_workspace')) 'new tools registered'
    $dir = Rpc 'list_directory' @{depth=2}
    Assert ((!$dir.error) -and ($dir.data.entries.Count -ge 3)) 'recursive directory listing'
    $info = Rpc 'get_file_info' @{path='sample.txt'}
    Assert ((!$info.error) -and ($info.data.lineCount -ge 2)) 'file metadata'
    $multi = Rpc 'read_multiple_files' @{paths=@('sample.txt','sub/match.txt')}
    Assert ((!$multi.error) -and ($multi.data.count -eq 2)) 'batch file reading'
    $bad = Rpc 'edit_block' @{filePath='sample.txt'; old_string='alpha'; new_string='changed'; expected_replacements=2}
    Assert $bad.error 'edit conflict rejection'
    Assert ([IO.File]::ReadAllText((Join-Path $workspace 'sample.txt')).Contains('alpha')) 'failed edit did not mutate file'
    $edit = Rpc 'edit_block' @{filePath='sample.txt'; old_string='alpha'; new_string='changed'}
    Assert ((!$edit.error) -and ($edit.data.replacements -eq 1)) 'atomic edit'
    $word = Rpc 'read_document' @{filePath='sample.docx'}
    Assert ((!$word.error) -and $word.data.text.Contains('Hello DOCX')) 'DOCX text extraction'
    $excel = Rpc 'read_document' @{filePath='sample.xlsx'}
    Assert ((!$excel.error) -and $excel.data.cells[0].value -eq 'Hello XLSX') 'XLSX first-sheet extraction'
    $search = Rpc 'search_workspace' @{pattern='needle'; searchType='content'}
    Assert ((!$search.error) -and ($search.data.matches.Count -eq 1)) 'content search'
    $names = Rpc 'search_workspace' @{pattern='sample'; searchType='files'}
    Assert ((!$names.error) -and ($names.data.matches.Count -ge 3)) 'filename search'
    $traversal = Rpc 'get_file_info' @{path='../outside.txt'}
    Assert $traversal.error 'outside workspace is rejected'
    $create = Rpc 'create_directory' @{path='new/nested'}
    Assert ((!$create.error) -and (Test-Path (Join-Path $workspace 'new\nested'))) 'recursive directory creation'
    $move = Rpc 'move_file' @{source='sub/match.txt'; destination='new/moved.txt'}
    Assert ((!$move.error) -and (Test-Path (Join-Path $workspace 'new\moved.txt'))) 'move_file'
    $procList = Rpc 'list_processes'
    Assert ((!$procList.error) -and ($procList.data.processes.Count -gt 0)) 'system process listing'
    $stats = Rpc 'get_usage_stats'
    Assert ((!$stats.error) -and ($stats.data.successfulCalls -gt 5)) 'usage statistics'
    $history = Rpc 'get_recent_tool_calls'
    Assert ((!$history.error) -and ($history.data.Count -gt 5)) 'redacted tool history'
    $terminal = Rpc 'run_terminal_command' @{command='echo mcp-smoke'; timeoutMs=5000}
    Assert ((!$terminal.error) -and $terminal.data.stdout.Contains('mcp-smoke')) 'terminal command execution'
    $denied = Rpc 'kill_process' @{pid=$PID;confirm=$false}
    Assert $denied.error 'process termination requires confirmation'
    $session = Rpc 'start_process' @{command='echo interactive-check';name='smoke-shell'}
    Assert ((!$session.error) -and (![string]::IsNullOrWhiteSpace($session.data.id))) 'persistent shell starts'
    Start-Sleep -Milliseconds 250
    $read = Rpc 'read_process_output' @{id=$session.data.id;offset=0;lines=100}
    Assert ((!$read.error) -and ($read.data.output -match 'interactive-check')) 'incremental shell output'
    $interaction = Rpc 'interact_with_process' @{id=$session.data.id;input='echo second-check'}
    Assert ((!$interaction.error) -and ($interaction.data.output -match 'second-check')) 'interactive shell input'
    $sessions = Rpc 'list_sessions'
    Assert ((!$sessions.error) -and ($sessions.data.Count -gt 0)) 'active shell sessions'
    $terminated = Rpc 'force_terminate' @{id=$session.data.id}
    Assert ((!$terminated.error) -and $terminated.data.killed) 'shell termination'
    $profileName = 'smoke-' + [guid]::NewGuid().ToString('N')
    $registered = Rpc 'register_device' @{name=$profileName;sshHost='example-alias';root='/srv/project'}
    Assert ((!$registered.error) -and $registered.data.registered) 'SSH profile registration'
    $devices = Rpc 'list_devices'
    Assert ((!$devices.error) -and ($devices.data.devices.name -contains $profileName)) 'SSH profile listing'
    $local = Rpc 'select_device' @{name='local'}
    Assert ((!$local.error) -and $local.data.mode -eq 'local') 'switch to local workspace'
    $removed = Rpc 'remove_device' @{name=$profileName}
    Assert ((!$removed.error) -and $removed.data.removed) 'SSH profile removal'
    Write-Output "ALL $passed ASSERTIONS PASSED"
} finally {
    if ($proc -and !$proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    $env:PATH = $originalPath
    $env:VSCODE_MCP_AUTH_TOKEN = $previousToken
    Remove-Item -LiteralPath $workspace -Recurse -Force -ErrorAction SilentlyContinue
}
