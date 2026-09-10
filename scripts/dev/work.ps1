# Windows PowerShell 5.1 entry point matching work.sh; no WSL dependency.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$script:DryRun = $false
$script:IssueNumber = ''; $script:IssueUrl = ''; $script:WorkBranch = ''; $script:WorktreePath = ''
$script:FailureCode = 1
$script:Resume = "& '$PSCommandPath' " + (($args | ForEach-Object { "'" + $_.Replace("'", "''") + "'" }) -join ' ')

function Get-Setting([string]$Name, [string]$Default) {
    $value = [Environment]::GetEnvironmentVariable($Name)
    if ($value) { return $value }; return $Default
}
$ProjectId = Get-Setting 'ZLINK_PROJECT_ID' 'PVT_kwHOErOLTs4Bi-bl'
$ProjectOwner = Get-Setting 'ZLINK_PROJECT_OWNER' 'zlink-systems'
$ProjectNumber = Get-Setting 'ZLINK_PROJECT_NUMBER' '1'
$ProjectField = Get-Setting 'ZLINK_PROJECT_STATUS_FIELD' 'Status'
$ProjectFieldId = Get-Setting 'ZLINK_PROJECT_STATUS_FIELD_ID' 'PVTSSF_lAHOErOLTs4Bi-blzhh0gcI'

function Invoke-Tool([string]$File, [string[]]$Arguments, [switch]$AllowFailure) {
    # Native stderr is diagnostic output, not the PowerShell 5.1 verdict.
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $output = @(& $File @Arguments); $code = $LASTEXITCODE }
    finally { $ErrorActionPreference = $saved }
    if ($code -ne 0 -and -not $AllowFailure) { throw "$File 실패 (exit $code): $($Arguments -join ' ')" }
    $script:ToolExitCode = $code
    return ($output -join "`n")
}
function Invoke-Change([string]$File, [string[]]$Arguments) {
    if ($script:DryRun) {
        Write-Host ('[dry-run] & ' + ((@($File) + $Arguments | ForEach-Object { "'" + $_.Replace("'", "''") + "'" }) -join ' '))
        return ''
    }
    return Invoke-Tool $File $Arguments
}
function Read-Gh([string[]]$Arguments) { return (Invoke-Tool gh $Arguments | ConvertFrom-Json) }
function Fail-Usage([string]$Message) { $script:FailureCode = 2; throw $Message }
function Require-File([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { Fail-Usage "파일이 없습니다: $Path" }
}
function Get-Root { return Invoke-Tool git @('rev-parse', '--show-toplevel') }
function Get-Branch { return Invoke-Tool git @('branch', '--show-current') }
function Get-IssueNumber([string]$Branch) {
    if ($Branch -match '^[^/]+/([1-9][0-9]*)-[a-z0-9-]+$') { return $Matches[1] }
    Fail-Usage "Issue 브랜치 형식이 아닙니다: $Branch"
}
function Read-Issue([string]$Number) {
    $issue = Read-Gh @('issue', 'view', $Number, '--json', 'number,title,url,labels,milestone,state')
    if (-not $issue.title -or -not $issue.url) { throw "Issue #$Number 정보를 읽지 못했습니다." }
    $script:IssueNumber = [string]$issue.number; $script:IssueUrl = $issue.url
    return $issue
}
function Get-Label($Issue, [string]$Prefix) {
    foreach ($label in $Issue.labels) { if ($label.name.StartsWith($Prefix)) { return $label.name.Substring($Prefix.Length).Trim() } }
    return ''
}
function Test-Area([string]$Area) {
    if ($Area -notin @('core','bindings','framework-dotnet','framework-java','framework-node','framework-cpp','bench','ci','docs')) {
        Fail-Usage "지원하지 않는 area: $Area"
    }
}
function Test-IssueBody([string]$Path) {
    Require-File $Path
    $found = @{}; $section = ''
    foreach ($line in Get-Content -LiteralPath $Path -Encoding UTF8) {
        $clean = ($line -replace '^\s*#{1,6}\s*', '') -replace '^\s*[-*]\s*', ''
        if ($clean -match '^(범위|완료\s*조건|근거)(?:\s*:\s*(.*)|\s*)$') {
            $section = $Matches[1] -replace '\s', ''
            if ($Matches.ContainsKey(2) -and $Matches[2].Trim()) { $found[$section] = $true }
        } elseif ($line -match '^\s*#{1,6}\s+') { $section = '' }
        elseif ($section -and $line.Trim() -and $line -notmatch '^\s*<!--') { $found[$section] = $true }
    }
    foreach ($name in @('범위','완료조건','근거')) { if (-not $found.ContainsKey($name)) { Fail-Usage "Issue 본문에 비어 있지 않은 '$name' 항목이 필요합니다." } }
}
function Get-Worktrees {
    $path = ''; $branch = ''
    foreach ($line in ((Invoke-Tool git @('worktree','list','--porcelain')) + "`n`n").Split("`n")) {
        if ($line.StartsWith('worktree ')) { $path = $line.Substring(9).TrimEnd("`r") }
        elseif ($line.StartsWith('branch refs/heads/')) { $branch = $line.Substring(18).TrimEnd("`r") }
        elseif (-not $line.Trim()) {
            if ($path) { [pscustomobject]@{ Path = $path; Branch = $branch } }
            $path = ''; $branch = ''
        }
    }
}
function Get-RemoteSha([string]$Branch) {
    $row = Invoke-Tool git @('ls-remote','--heads','origin',"refs/heads/$Branch")
    if ($row) { return ($row -split '\s+')[0] }; return ''
}
function Set-ProjectStatus([string]$Url, [string]$Status) {
    try {
        $items = Read-Gh @('project','item-list',$ProjectNumber,'--owner',$ProjectOwner,'--limit','1000','--format','json')
        $item = @($items.items | Where-Object { $_.content.PSObject.Properties['url'] -and $_.content.url -eq $Url } | Select-Object -First 1)
        if ($item.Count) { $itemId = $item[0].id }
        else {
            $added = Invoke-Change gh @('project','item-add',$ProjectNumber,'--owner',$ProjectOwner,'--url',$Url,'--format','json')
            $itemId = if ($script:DryRun) { '<new-item-id>' } else { ($added | ConvertFrom-Json).id }
        }
        $fields = Read-Gh @('project','field-list',$ProjectNumber,'--owner',$ProjectOwner,'--format','json')
        $field = @($fields.fields | Where-Object { $_.id -eq $ProjectFieldId -or $_.name -eq $ProjectField } | Select-Object -First 1)
        if (-not $field.Count) { throw 'Project Status 필드가 없습니다.' }
        $option = @($field[0].options | Where-Object { $_.name -eq $Status } | Select-Object -First 1)
        if (-not $option.Count) { throw "Project Status 옵션이 없습니다: $Status" }
        Invoke-Change gh @('project','item-edit','--project-id',$ProjectId,'--id',$itemId,'--field-id',$field[0].id,'--single-select-option-id',$option[0].id) | Out-Host
    } catch { Write-Warning "Project '$Status' 갱신 실패; 로컬 작업은 계속합니다: $_" }
}
function Set-Milestone($Issue, [string]$Name) {
    if (-not $Name) { return }
    try {
        if ($null -eq $Issue.milestone -or $Issue.milestone.title -ne $Name) {
            Invoke-Change gh @('issue','edit',[string]$Issue.number,'--milestone',$Name) | Out-Host
        }
    } catch { Write-Warning "milestone 갱신 실패; 로컬 작업은 계속합니다: $_" }
}
function Read-Options([string[]]$Values, [string[]]$Valued, [string[]]$Flags, [switch]$Title) {
    $result = @{}; $result['title'] = ''
    for ($i = 0; $i -lt $Values.Count; $i++) {
        $key = $Values[$i]
        if ($key -in $Valued) {
            if ($i + 1 -ge $Values.Count) { Fail-Usage "$key 값이 필요합니다." }
            $result[$key] = $Values[++$i]
        } elseif ($key -in $Flags) {
            if ($result.ContainsKey($key)) { Fail-Usage "중복 옵션: $key" }; $result[$key] = $true
        } elseif ($Title -and -not $key.StartsWith('--') -and -not $result.title) { $result.title = $key }
        else { Fail-Usage "알 수 없는 인자: $key" }
    }
    return $result
}
function Start-Work([string[]]$Values) {
    $o = Read-Options $Values @('--issue','--area','--kind','--body','--milestone') @('--no-packages') -Title
    $root = Get-Root
    if ($o['--issue']) {
        if ($o['--issue'] -notmatch '^[1-9][0-9]*$') { Fail-Usage 'Issue 번호가 올바르지 않습니다.' }
        if ($o.title -or $o['--area'] -or $o['--kind'] -or $o['--body'] -or $o['--milestone']) { Fail-Usage '--issue에는 제목/area/kind/body/milestone을 함께 지정할 수 없습니다.' }
        $issue = Read-Issue $o['--issue']; $area = Get-Label $issue 'area:'; Test-Area $area
    } else {
        if (-not ($o.title -and $o['--area'] -and $o['--kind'] -and $o['--body'])) { Fail-Usage '제목, --area, --kind, --body가 필요합니다.' }
        $area = $o['--area']; Test-Area $area
        if ($o['--kind'] -notin @('bug','perf','feature','chore')) { Fail-Usage '지원하지 않는 kind입니다.' }
        Test-IssueBody $o['--body']
        $issues = @(Read-Gh @('issue','list','--state','all','--limit','100','--search',"$($o.title) in:title",'--json','number,title,url'))
        $found = @($issues | Where-Object { $_.title -ceq $o.title } | Select-Object -First 1)
        if ($found.Count) {
            $issue = Read-Issue ([string]$found[0].number)
            if ((Get-Label $issue 'area:') -ne $area) { throw '같은 제목의 Issue가 다른 area에 있습니다.' }
            $kind = Get-Label $issue 'kind:'
            if ($kind -and $kind -ne $o['--kind']) { throw '같은 제목의 Issue가 다른 kind에 있습니다.' }
            Write-Host "Issue 재사용: #$($issue.number)"
        } else {
            $url = Invoke-Change gh @('issue','create','--title',$o.title,'--body-file',$o['--body'],'--label',"area: $area",'--label',"kind: $($o['--kind'])")
            if ($script:DryRun) { Write-Host '새 Issue 번호는 실행 뒤 정해집니다. 이후 단계는 생략합니다.'; return }
            $number = ($url.Trim() -split '/')[-1]
            if ($number -notmatch '^[1-9][0-9]*$') { throw "Issue URL에서 번호를 읽지 못했습니다: $url" }
            $issue = Read-Issue $number
        }
        if (-not (Get-Label $issue 'kind:')) { Invoke-Change gh @('issue','edit',[string]$issue.number,'--add-label',"kind: $($o['--kind'])") | Out-Host }
    }
    $milestone = $o['--milestone']; $conf = Join-Path $root 'scripts/dev/work.conf'
    if (-not $milestone -and (Test-Path -LiteralPath $conf)) {
        $line = Get-Content -LiteralPath $conf | Where-Object { $_ -match '^\s*current_milestone\s*=' } | Select-Object -First 1
        if ($line) { $milestone = (($line -split '=',2)[1] -replace '\s*#.*$','').Trim().Trim('"') }
    }
    Set-Milestone $issue $milestone
    $slug = ($issue.title.ToLowerInvariant() -replace '[^a-z0-9]+','-').Trim('-')
    if ($slug.Length -gt 40) { $slug = $slug.Substring(0,40).TrimEnd('-') }; if (-not $slug) { $slug = 'work' }
    $pattern = '^[^/]+/' + $issue.number + '-[a-z0-9-]+$'
    $trees = @(Get-Worktrees); $branch = Get-Branch
    if ($branch -notmatch $pattern) {
        $branch = @($trees | Where-Object { $_.Branch -match $pattern } | ForEach-Object Branch | Select-Object -First 1) -join ''
    }
    if (-not $branch) {
        $branch = @((Invoke-Tool git @('for-each-ref','--format=%(refname:short)','refs/heads','refs/remotes/origin')).Split("`n") | ForEach-Object { $_ -replace '^origin/','' } | Where-Object { $_ -match $pattern } | Select-Object -First 1) -join ''
    }
    if (-not $branch) { $branch = "$area/$($issue.number)-$slug" }
    elseif ($branch -ne "$area/$($issue.number)-$slug") { Write-Warning "기존 Issue 브랜치 '$branch'가 canonical 이름과 다르지만 재사용합니다." }
    $script:WorkBranch = $branch
    $existing = @($trees | Where-Object { $_.Branch -eq $branch } | Select-Object -First 1)
    if ($existing.Count) { $target = $existing[0].Path; Write-Host "worktree 재사용: $target" }
    else {
        # Windows worktrees are siblings of the primary checkout, not WSL paths.
        $parent = Get-Setting 'ZLINK_WORKTREE_ROOT' (Split-Path ([IO.Path]::GetFullPath($trees[0].Path)) -Parent)
        $target = [IO.Path]::GetFullPath((Join-Path $parent "zlink-$($issue.number)-$slug"))
        if (Test-Path -LiteralPath $target) { throw "등록되지 않은 경로가 이미 존재합니다: $target" }
        Invoke-Tool git @('show-ref','--verify','--quiet',"refs/heads/$branch") -AllowFailure | Out-Null
        if ($script:ToolExitCode -eq 0) { Invoke-Change git @('worktree','add',$target,$branch) | Out-Host }
        else {
            Invoke-Change git @('fetch','origin') | Out-Host
            Invoke-Tool git @('show-ref','--verify','--quiet',"refs/remotes/origin/$branch") -AllowFailure | Out-Null
            $base = if ($script:ToolExitCode -eq 0) { "origin/$branch" } else { 'origin/main' }
            Invoke-Change git @('worktree','add',$target,'-b',$branch,$base) | Out-Host
        }
    }
    $script:WorktreePath = $target
    if (-not $o['--no-packages']) {
        $prefix = Get-Setting 'ZLINK_CORE_PACKAGE_PREFIX' ''
        if (-not $prefix) {
            $fetched = Invoke-Change powershell @('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',"$target/scripts/local-package/core/fetch-release.ps1")
            $prefix = if ($script:DryRun) { '<downloaded-Core-prefix>' } else { ($fetched.Trim() -split "`n")[-1].Trim() }
        }
        Invoke-Change powershell @('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',"$target/scripts/local-package/build-windows.ps1",'-RepositoryRoot',$target,'-CorePrefix',$prefix) | Out-Host
    } else { Write-Host '패키지 준비 건너뜀 (--no-packages)' }
    Set-ProjectStatus $issue.url 'In Progress'
    Write-Host "시작 완료: Issue #$($issue.number)`n브랜치: $branch`nworktree: $target"
}
function Submit-Pr([string[]]$Values) {
    $o = Read-Options $Values @('--body') @('--closes','--refs')
    if (-not $o['--body'] -or ($o['--closes'] -and $o['--refs'])) { Fail-Usage '--body가 필요하며 --closes/--refs 중 하나만 지정해야 합니다.' }
    $root = Get-Root; $branch = Get-Branch; $number = Get-IssueNumber $branch
    $script:WorkBranch = $branch; $script:WorktreePath = $root; $script:IssueNumber = $number
    Require-File $o['--body']; $body = Get-Content -LiteralPath $o['--body'] -Encoding UTF8
    $mode = if ($o['--closes']) { 'Closes' } else { 'Refs' }
    if ($body[0] -cne "$mode #$number") { Fail-Usage "PR 첫 줄은 '$mode #$number'여야 합니다." }
    if (-not ($body -match '^\s{0,3}#{1,6}\s+검증(?:\s|:|—|-|$)')) { Fail-Usage "PR에 '검증' 절이 필요합니다." }
    $title = Invoke-Tool git @('log','-1','--pretty=%s')
    if ($title -notmatch '^[^:\s][^:]*:\s+.+$') { Fail-Usage '최신 커밋 제목은 <모듈>: <요약> 형식이어야 합니다.' }
    $sha = Invoke-Tool git @('rev-parse','HEAD')
    if ((Get-RemoteSha $branch) -ne $sha) { Invoke-Change git @('push','-u','origin',$branch) | Out-Host }
    $prs = @(Read-Gh @('pr','list','--head',$branch,'--state','open','--limit','1','--json','number'))
    if ($prs.Count) {
        $pr = Read-Gh @('pr','view',[string]$prs[0].number,'--json','baseRefName,url')
        if ($pr.baseRefName -ne 'main') { throw '기존 PR base가 main이 아닙니다.' }
        Invoke-Change gh @('pr','edit',[string]$prs[0].number,'--title',$title,'--body-file',$o['--body'],'--base','main') | Out-Host
        Write-Host $pr.url
    } else { Invoke-Change gh @('pr','create','--base','main','--head',$branch,'--title',$title,'--body-file',$o['--body']) | Out-Host }
    $issue = Read-Issue $number; Set-ProjectStatus $issue.url 'Review'
}
function Finish-Work([string[]]$Values) {
    $o = Read-Options $Values @('--verified') @()
    if (-not $o['--verified'] -or $o['--verified'] -notmatch '^[0-9a-fA-F]{40}$') { Fail-Usage '--verified에는 40자리 SHA가 필요합니다.' }
    $verified = $o['--verified'].ToLowerInvariant(); $branch = Get-Branch; $number = Get-IssueNumber $branch; $root = Get-Root
    $script:WorkBranch = $branch; $script:IssueNumber = $number; $script:WorktreePath = $root
    $prs = @(Read-Gh @('pr','list','--head',$branch,'--state','all','--limit','1','--json','number'))
    if (-not $prs.Count) { throw '브랜치의 PR을 찾지 못했습니다.' }
    $prNumber = [string]$prs[0].number
    $pr = Read-Gh @('pr','view',$prNumber,'--json','state,headRefOid,baseRefName,body')
    if ($pr.baseRefName -ne 'main' -or $pr.headRefOid -ne $verified) { throw 'PR base 또는 검증 SHA가 PR HEAD와 다릅니다.' }
    $first = ($pr.body -split "`n")[0].TrimEnd("`r")
    if ($first -notin @("Closes #$number", "Refs #$number")) { throw 'PR 첫 줄이 해당 Issue를 Closes/Refs 하지 않습니다.' }
    $closes = $first -eq "Closes #$number"
    if ($closes) {
        $trees = @(Get-Worktrees); $primary = [IO.Path]::GetFullPath($trees[0].Path); $target = [IO.Path]::GetFullPath($root)
        if ($primary -eq $target -or -not (@($trees | Where-Object { [IO.Path]::GetFullPath($_.Path) -eq $target -and $_.Branch -eq $branch }).Count)) { throw '등록된 Issue worktree만 정리할 수 있습니다.' }
        if (Invoke-Tool git @('-C',$root,'status','--porcelain')) { throw 'worktree에 커밋되지 않은 변경이 있습니다.' }
        $remote = Get-RemoteSha $branch; $local = Invoke-Tool git @('rev-parse',$branch)
        if ((-not $remote -and $pr.state -ne 'MERGED') -or ($remote -and $remote -ne $local)) { throw '원격에 push되지 않은 커밋이 있거나 원격 상태를 검증할 수 없습니다.' }
    }
    if ($pr.state -eq 'OPEN') { Invoke-Change gh @('pr','merge',$prNumber,'--merge','--match-head-commit',$verified) | Out-Host }
    elseif ($pr.state -ne 'MERGED') { throw "merge할 수 없는 PR 상태: $($pr.state)" }
    if (-not $closes) { Write-Host 'Refs PR: Issue와 worktree를 유지합니다.'; return }
    if (Get-RemoteSha $branch) { Invoke-Change git @('push','origin','--delete',$branch) | Out-Host }
    $issue = Read-Issue $number
    # Change cwd before removing the registered worktree. No shell-composed deletion.
    if (-not $script:DryRun) { Set-Location -LiteralPath $primary }
    Invoke-Change git @('-C',$primary,'worktree','remove',$target) | Out-Host
    Set-ProjectStatus $issue.url 'Done'
    Write-Host '정리 완료: 원격 브랜치와 worktree 제거, Project Done 갱신.'
}
function Show-Status([string[]]$Values) {
    $o = Read-Options $Values @('--milestone') @(); Get-Root | Out-Null
    Write-Host "worktree`t브랜치`tIssue`tPR`tCI"
    foreach ($tree in Get-Worktrees) {
        if (-not $tree.Branch) { continue }
        $state = '-'; $cell = '-'; $checks = '-'
        if ($tree.Branch -match '^[^/]+/([1-9][0-9]*)-[a-z0-9-]+$') {
            $number = $Matches[1]
            try {
                $issue = Read-Gh @('issue','view',$number,'--json','state'); $state = "#$number $($issue.state)"
                $prs = @(Read-Gh @('pr','list','--head',$tree.Branch,'--state','all','--limit','1','--json','number,state'))
                if ($prs.Count) {
                    $cell = "#$($prs[0].number) $($prs[0].state)"
                    $pr = Read-Gh @('pr','view',[string]$prs[0].number,'--json','statusCheckRollup')
                    $rollup = @($pr.statusCheckRollup)
                    $outcomes = @(); $states = @()
                    foreach ($check in $rollup) {
                        $outcomes += if ($check.PSObject.Properties['conclusion'] -and $null -ne $check.conclusion) { $check.conclusion } elseif ($check.PSObject.Properties['state']) { $check.state } else { '' }
                        $states += if ($check.PSObject.Properties['status']) { $check.status } elseif ($check.PSObject.Properties['state']) { $check.state } else { '' }
                    }
                    $checks = if (-not $rollup.Count) { '없음' } elseif (-not @($outcomes | Where-Object { $_ -notin @('SUCCESS','SKIPPED') }).Count) { 'PASS' } elseif (@($states | Where-Object { $_ -in @('PENDING','QUEUED','IN_PROGRESS') }).Count) { '대기' } else { '실패' }
                }
            } catch { $checks = '조회 실패'; Write-Warning $_ }
        }
        Write-Host "$($tree.Path)`t$($tree.Branch)`t$state`t$cell`t$checks"
    }
    if ($o['--milestone']) {
        $issues = @(Read-Gh @('issue','list','--milestone',$o['--milestone'],'--state','all','--limit','100','--json','number,title,state,url'))
        $passed = $true; Write-Host "milestone $($o['--milestone'])`nIssue`t상태`tCloses merge PR`torigin/main 포함"
        foreach ($issue in $issues) {
            $found = @(); $included = $false
            try { $found = @(Read-Gh @('pr','list','--state','merged','--search',"#$($issue.number)",'--limit','100','--json','number,mergeCommit,body') | Where-Object { ($_.body -split "`n")[0] -eq "Closes #$($issue.number)" } | Select-Object -First 1) }
            catch { Write-Warning $_ }
            if ($found.Count -and $found[0].mergeCommit) {
                Invoke-Tool git @('merge-base','--is-ancestor',$found[0].mergeCommit.oid,'origin/main') -AllowFailure | Out-Null
                $included = $script:ToolExitCode -eq 0
            }
            if ($issue.state -ne 'CLOSED' -or -not $found.Count -or -not $included) { $passed = $false }
            $prCell = if ($found.Count) { "#$($found[0].number)" } else { '-' }
            Write-Host "#$($issue.number)`t$($issue.state)`t$prCell`t$included"
        }
        if (-not $passed) { throw '판정: 릴리스 조건 미충족' }; Write-Host '판정: 릴리스 조건 충족'
    }
}
try {
    foreach ($tool in @('git','gh')) { if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "필요한 명령: $tool" } }
    $values = @($args | Where-Object { if ($_ -eq '--dry-run') { $script:DryRun = $true; $false } else { $true } })
    if (-not $values.Count) { Fail-Usage '사용법: work.ps1 [--dry-run] start|status|pr|done [옵션]' }
    $tail = @($values | Select-Object -Skip 1)
    switch ($values[0]) {
        'start' { Start-Work $tail }
        'pr' { Submit-Pr $tail }
        'done' { Finish-Work $tail }
        'status' { Show-Status $tail }
        { $_ -in @('help','--help','-h') } {
            Write-Host 'work.ps1 [--dry-run] start "제목" --area AREA --kind KIND --body FILE [--milestone NAME] [--no-packages]'
            Write-Host 'work.ps1 [--dry-run] start --issue N [--no-packages]'
            Write-Host 'work.ps1 [--dry-run] pr --body FILE [--closes|--refs]'
            Write-Host 'work.ps1 [--dry-run] status [--milestone NAME]'
            Write-Host 'work.ps1 [--dry-run] done --verified SHA'
        }
        default { Fail-Usage "알 수 없는 명령: $($values[0])" }
    }
} catch {
    [Console]::Error.WriteLine("오류: $_")
    [Console]::Error.WriteLine("Issue: $script:IssueNumber $script:IssueUrl; 브랜치: $script:WorkBranch; worktree: $script:WorktreePath")
    [Console]::Error.WriteLine("재개 명령: $script:Resume")
    exit $script:FailureCode
}
