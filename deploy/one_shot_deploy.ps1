[CmdletBinding()]
param(
  [string]$ProjectPrefix,
  [string]$Region = "us-east-1",
  [string]$KeyPairName,
  [switch]$ForceRecreateEc2,
  [switch]$SkipEc2,
  [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Write-Section([string]$Name) {
  Write-Host ""
  Write-Host "=== $Name ===" -ForegroundColor Cyan
}

function Require-Command([string]$Name) {
  if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
    throw "Required command not found: $Name"
  }
}

function To-Plain([Security.SecureString]$Secure) {
  $ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($Secure)
  try { return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr) }
  finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr) }
}

function Sha256Hex([string]$Text) {
  $sha = [System.Security.Cryptography.SHA256]::Create()
  try {
    $bytes = [Text.Encoding]::UTF8.GetBytes($Text)
    $hash = $sha.ComputeHash($bytes)
    return ($hash | ForEach-Object { $_.ToString("x2") }) -join ""
  } finally {
    $sha.Dispose()
  }
}

function New-HexToken([int]$Bytes = 32) {
  if ($Bytes -le 0) { throw "Bytes must be > 0" }
  $buf = New-Object byte[] $Bytes
  $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
  try {
    $rng.GetBytes($buf)
  } finally {
    $rng.Dispose()
  }
  return ($buf | ForEach-Object { $_.ToString("x2") }) -join ""
}

function Prompt-Text([string]$Prompt, [string]$Default = "") {
  if ($Default) {
    $v = Read-Host "$Prompt [$Default]"
    if ([string]::IsNullOrWhiteSpace($v)) { return $Default }
    return $v.Trim()
  }
  while ($true) {
    $v = Read-Host $Prompt
    if (-not [string]::IsNullOrWhiteSpace($v)) { return $v.Trim() }
  }
}

function Invoke-AwsJson([string[]]$Args) {
  $all = @($Args + @("--region", $Region, "--output", "json"))
  if ($DryRun) {
    Write-Host "[DryRun] aws $($all -join ' ')" -ForegroundColor Yellow
    return $null
  }
  $out = & aws @all 2>&1
  if ($LASTEXITCODE -ne 0) {
    throw "AWS command failed: aws $($all -join ' ')`n$out"
  }
  if ([string]::IsNullOrWhiteSpace($out)) { return $null }
  return $out | ConvertFrom-Json
}

function Invoke-AwsText([string[]]$Args) {
  $all = @($Args + @("--region", $Region, "--output", "text"))
  if ($DryRun) {
    Write-Host "[DryRun] aws $($all -join ' ')" -ForegroundColor Yellow
    return ""
  }
  $out = & aws @all 2>&1
  if ($LASTEXITCODE -ne 0) {
    throw "AWS command failed: aws $($all -join ' ')`n$out"
  }
  return ($out | Out-String).Trim()
}

function Invoke-AwsNoOut([string[]]$Args, [switch]$IgnoreDuplicateRule) {
  $all = @($Args + @("--region", $Region))
  if ($DryRun) {
    Write-Host "[DryRun] aws $($all -join ' ')" -ForegroundColor Yellow
    return
  }
  $out = & aws @all 2>&1
  if ($LASTEXITCODE -ne 0) {
    if ($IgnoreDuplicateRule -and ($out -match "InvalidPermission\.Duplicate")) {
      return
    }
    throw "AWS command failed: aws $($all -join ' ')`n$out"
  }
}

function Ensure-TimestreamDatabase([string]$DbName) {
  Write-Host "Ensuring Timestream DB: $DbName"
  $exists = $true
  try {
    Invoke-AwsNoOut @("timestream-write", "describe-database", "--database-name", $DbName)
  } catch {
    $exists = $false
  }
  if (-not $exists) {
    Invoke-AwsNoOut @("timestream-write", "create-database", "--database-name", $DbName)
  }
}

function Ensure-TimestreamTable([string]$DbName, [string]$TableName) {
  Write-Host "Ensuring Timestream table: $TableName"
  $exists = $true
  try {
    Invoke-AwsNoOut @("timestream-write", "describe-table", "--database-name", $DbName, "--table-name", $TableName)
  } catch {
    $exists = $false
  }
  if (-not $exists) {
    Invoke-AwsNoOut @(
      "timestream-write", "create-table",
      "--database-name", $DbName,
      "--table-name", $TableName,
      "--retention-properties", "MemoryStoreRetentionPeriodInHours=72,MagneticStoreRetentionPeriodInDays=365"
    )
  } else {
    Invoke-AwsNoOut @(
      "timestream-write", "update-table",
      "--database-name", $DbName,
      "--table-name", $TableName,
      "--retention-properties", "MemoryStoreRetentionPeriodInHours=72,MagneticStoreRetentionPeriodInDays=365"
    )
  }
}

function Ensure-IamRoleAndProfile([string]$RoleName, [string]$ProfileName, [string]$PolicyName, [string]$PolicyFile) {
  Write-Host "Ensuring IAM role/profile for EC2"
  $trustDoc = @"
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Principal": {"Service": "ec2.amazonaws.com"},
      "Action": "sts:AssumeRole"
    }
  ]
}
"@
  $tmpTrust = Join-Path $env:TEMP "chamber-trust-$RoleName.json"
  Set-Content -Path $tmpTrust -Value $trustDoc -NoNewline

  $roleExists = $true
  try {
    Invoke-AwsNoOut @("iam", "get-role", "--role-name", $RoleName)
  } catch {
    $roleExists = $false
  }
  if (-not $roleExists) {
    Invoke-AwsNoOut @("iam", "create-role", "--role-name", $RoleName, "--assume-role-policy-document", "file://$tmpTrust")
  }

  Invoke-AwsNoOut @("iam", "put-role-policy", "--role-name", $RoleName, "--policy-name", $PolicyName, "--policy-document", "file://$PolicyFile")

  $profileExists = $true
  try {
    Invoke-AwsNoOut @("iam", "get-instance-profile", "--instance-profile-name", $ProfileName)
  } catch {
    $profileExists = $false
  }
  if (-not $profileExists) {
    Invoke-AwsNoOut @("iam", "create-instance-profile", "--instance-profile-name", $ProfileName)
  }

  try {
    Invoke-AwsNoOut @("iam", "add-role-to-instance-profile", "--instance-profile-name", $ProfileName, "--role-name", $RoleName)
  } catch {
    if (-not $_.Exception.Message.Contains("LimitExceeded") -and -not $_.Exception.Message.Contains("already")) {
      throw
    }
  }

  if (-not $DryRun) { Start-Sleep -Seconds 10 }
}

function Ensure-SecurityGroup([string]$GroupName, [string]$VpcId, [string]$Cidr) {
  Write-Host "Ensuring security group: $GroupName"
  $sgId = ""
  try {
    $sgId = Invoke-AwsText @("ec2", "describe-security-groups", "--filters", "Name=group-name,Values=$GroupName", "Name=vpc-id,Values=$VpcId", "--query", "SecurityGroups[0].GroupId")
  } catch { $sgId = "" }

  if ([string]::IsNullOrWhiteSpace($sgId) -or $sgId -eq "None") {
    $sgId = Invoke-AwsText @("ec2", "create-security-group", "--group-name", $GroupName, "--description", "Streamlit SG for $GroupName", "--vpc-id", $VpcId, "--query", "GroupId")
  }

  Invoke-AwsNoOut @("ec2", "authorize-security-group-ingress", "--group-id", $sgId, "--protocol", "tcp", "--port", "22", "--cidr", $Cidr) -IgnoreDuplicateRule
  Invoke-AwsNoOut @("ec2", "authorize-security-group-ingress", "--group-id", $sgId, "--protocol", "tcp", "--port", "8501", "--cidr", $Cidr) -IgnoreDuplicateRule

  return $sgId
}

function New-UserDataFile(
  [string]$TemplatePath,
  [string]$OutPath,
  [string]$DashboardAppPath,
  [string]$RequirementsPath,
  [string]$AwsRegion,
  [string]$TsDb,
  [string]$TsTele,
  [string]$TsEvt,
  [string]$AppUser,
  [string]$AppHash
) {
  $tpl = Get-Content $TemplatePath -Raw
  $app = Get-Content $DashboardAppPath -Raw
  $req = Get-Content $RequirementsPath -Raw
  $appB64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($app))
  $reqB64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($req))

  $content = $tpl
  $content = $content.Replace("__AWS_REGION__", $AwsRegion)
  $content = $content.Replace("__TS_DB__", $TsDb)
  $content = $content.Replace("__TS_TABLE_TELEMETRY__", $TsTele)
  $content = $content.Replace("__TS_TABLE_EVENTS__", $TsEvt)
  $content = $content.Replace("__APP_USER__", $AppUser)
  $content = $content.Replace("__APP_PASSWORD_SHA256__", $AppHash)
  $content = $content.Replace("__APP_B64__", $appB64)
  $content = $content.Replace("__REQ_B64__", $reqB64)

  Set-Content -Path $OutPath -Value $content -NoNewline
}

function Ensure-EC2Instance(
  [string]$NameTag,
  [string]$KeyPair,
  [string]$SgId,
  [string]$InstanceProfile,
  [string]$UserDataFile,
  [string]$InstanceType,
  [switch]$ForceRecreate
) {
  Write-Host "Ensuring EC2 instance: $NameTag"

  $instanceId = Invoke-AwsText @(
    "ec2", "describe-instances",
    "--filters",
    "Name=tag:Name,Values=$NameTag",
    "Name=instance-state-name,Values=pending,running,stopping,stopped",
    "--query", "Reservations[].Instances[0].InstanceId"
  )

  if ($ForceRecreate -and $instanceId -and $instanceId -ne "None") {
    Invoke-AwsNoOut @("ec2", "terminate-instances", "--instance-ids", $instanceId)
    if (-not $DryRun) {
      & aws ec2 wait instance-terminated --instance-ids $instanceId --region $Region
      $instanceId = ""
    }
  }

  if ([string]::IsNullOrWhiteSpace($instanceId) -or $instanceId -eq "None") {
    $amiId = Invoke-AwsText @("ssm", "get-parameter", "--name", "/aws/service/ami-amazon-linux-latest/al2023-ami-kernel-default-x86_64", "--query", "Parameter.Value")
    $vpcId = Invoke-AwsText @("ec2", "describe-vpcs", "--filters", "Name=isDefault,Values=true", "--query", "Vpcs[0].VpcId")
    if (-not $vpcId -or $vpcId -eq "None") { throw "No default VPC found" }
    $subnetId = Invoke-AwsText @("ec2", "describe-subnets", "--filters", "Name=vpc-id,Values=$vpcId", "Name=default-for-az,Values=true", "--query", "Subnets[0].SubnetId")
    if (-not $subnetId -or $subnetId -eq "None") {
      $subnetId = Invoke-AwsText @("ec2", "describe-subnets", "--filters", "Name=vpc-id,Values=$vpcId", "--query", "Subnets[0].SubnetId")
    }

    $run = Invoke-AwsJson @(
      "ec2", "run-instances",
      "--image-id", $amiId,
      "--instance-type", $InstanceType,
      "--key-name", $KeyPair,
      "--security-group-ids", $SgId,
      "--subnet-id", $subnetId,
      "--iam-instance-profile", "Name=$InstanceProfile",
      "--user-data", "file://$UserDataFile",
      "--tag-specifications", "ResourceType=instance,Tags=[{Key=Name,Value=$NameTag}]",
      "--count", "1"
    )

    if (-not $DryRun) {
      $instanceId = $run.Instances[0].InstanceId
    } else {
      $instanceId = "i-dryrun"
    }
  } else {
    $state = Invoke-AwsText @("ec2", "describe-instances", "--instance-ids", $instanceId, "--query", "Reservations[0].Instances[0].State.Name")
    if ($state -eq "stopped") {
      Invoke-AwsNoOut @("ec2", "start-instances", "--instance-ids", $instanceId)
    }
  }

  if (-not $DryRun) {
    & aws ec2 wait instance-running --instance-ids $instanceId --region $Region
    & aws ec2 wait instance-status-ok --instance-ids $instanceId --region $Region
  }

  return $instanceId
}

function Test-ApiEndpoint([string]$ApiUrl, [string]$ApiToken, [string]$DeviceId) {
  if ($DryRun) { return @{ ok = $true; status = 0 } }
  $uri = "$ApiUrl/telemetry/batch"
  $headers = @{
    "X-Api-Token" = $ApiToken
    "X-Device-Id" = $DeviceId
    "Content-Type" = "application/json"
  }
  $body = '{"device_id":"' + $DeviceId + '","records":[]}'
  try {
    $resp = Invoke-WebRequest -Method Post -Uri $uri -Headers $headers -Body $body -TimeoutSec 20
    return @{ ok = $true; status = [int]$resp.StatusCode }
  } catch {
    return @{ ok = $false; status = -1; err = $_.Exception.Message }
  }
}

Write-Section "Preflight"
if (-not $DryRun) {
  Require-Command "aws"
  Require-Command "sam"
  Require-Command "python"
  if (-not $SkipEc2) { Require-Command "ssh" }
}

$ProjectPrefix = if ($ProjectPrefix) { $ProjectPrefix } else { Prompt-Text "Project prefix (ex: chamber-prod)" }
$KeyPairName = if ($KeyPairName) { $KeyPairName } else { Prompt-Text "Existing EC2 key pair name" }
$DeviceId = Prompt-Text "Device ID" "MEGA001"
$StreamlitUser = Prompt-Text "Streamlit username" "admin"
$WifiSsid = Prompt-Text "Wi-Fi SSID for firmware CFG output"
$WifiPassRaw = Read-Host "Wi-Fi password for firmware CFG output (leave blank for open Wi-Fi)"
$WifiPass = if ($null -eq $WifiPassRaw) { "" } else { $WifiPassRaw.Trim() }

$apiTokenSecure = Read-Host "API token (hidden input; leave blank to auto-generate)" -AsSecureString
$streamlitPassSecure = Read-Host "Streamlit password (hidden input)" -AsSecureString
$ApiToken = To-Plain $apiTokenSecure
$StreamlitPassword = To-Plain $streamlitPassSecure
if ([string]::IsNullOrWhiteSpace($ApiToken)) {
  $ApiToken = New-HexToken 32
  Write-Host "API token auto-generated for this deployment." -ForegroundColor Yellow
}
if ([string]::IsNullOrWhiteSpace($StreamlitPassword)) { throw "Streamlit password is required" }
$StreamlitPasswordSha256 = Sha256Hex $StreamlitPassword

$allowCidr = ""
try {
  $ipData = Invoke-RestMethod -Uri "https://api.ipify.org?format=json" -TimeoutSec 10
  if ($ipData.ip) {
    $allowCidr = "$($ipData.ip)/32"
  }
} catch {
  $allowCidr = ""
}
if ([string]::IsNullOrWhiteSpace($allowCidr)) {
  $allowCidr = Prompt-Text "Could not auto-detect IP. Enter CIDR to allow (ex: 1.2.3.4/32)"
}
Write-Host "Allow CIDR: $allowCidr"

if (-not $DryRun) {
  $identityJson = Invoke-AwsJson @("sts", "get-caller-identity")
  Write-Host "AWS Account: $($identityJson.Account)"
}

$ScriptRoot = $PSScriptRoot
$ProjectRoot = Split-Path -Parent $ScriptRoot
$LambdaDir = Join-Path $ProjectRoot "cloud\lambda_ingest"
$DashboardDir = Join-Path $ProjectRoot "dashboard"
$TemplatePath = Join-Path $ScriptRoot "streamlit_user_data.sh.tpl"
$UserDataOut = Join-Path $env:TEMP ("chamber-user-data-" + $ProjectPrefix + ".sh")
$PolicyPath = Join-Path $ScriptRoot "policies\ec2_timestream_read_policy.json"
$DashboardAppPath = Join-Path $DashboardDir "streamlit_app.py"
$DashboardReqPath = Join-Path $DashboardDir "requirements.txt"

$dbName = "$ProjectPrefix-chamber"
$teleTable = "$ProjectPrefix-telemetry"
$evtTable = "$ProjectPrefix-events"
$stackName = "$ProjectPrefix-ingest"
$ec2Name = "$ProjectPrefix-streamlit"
$roleName = "$ProjectPrefix-streamlit-role"
$profileName = "$ProjectPrefix-streamlit-profile"
$policyName = "$ProjectPrefix-streamlit-timestream-read"
$sgName = "$ProjectPrefix-streamlit-sg"

Write-Section "Timestream"
Ensure-TimestreamDatabase -DbName $dbName
Ensure-TimestreamTable -DbName $dbName -TableName $teleTable
Ensure-TimestreamTable -DbName $dbName -TableName $evtTable

Write-Section "Lambda + API (SAM)"
if ($DryRun) {
  Write-Host "[DryRun] sam build" -ForegroundColor Yellow
  Write-Host "[DryRun] sam deploy --stack-name $stackName ..." -ForegroundColor Yellow
  $apiUrl = "https://dryrun.execute-api.$Region.amazonaws.com/v1"
  $lambdaName = "$stackName-function"
  $httpApiId = "dryrunapi"
} else {
  Push-Location $LambdaDir
  try {
    & sam build
    if ($LASTEXITCODE -ne 0) { throw "sam build failed" }
    & sam deploy `
      --stack-name $stackName `
      --resolve-s3 `
      --capabilities CAPABILITY_IAM `
      --no-confirm-changeset `
      --no-fail-on-empty-changeset `
      --region $Region `
      --parameter-overrides `
      "ApiToken=$ApiToken" `
      "TsDatabase=$dbName" `
      "TsTableTelemetry=$teleTable" `
      "TsTableEvents=$evtTable"
    if ($LASTEXITCODE -ne 0) { throw "sam deploy failed" }
  } finally {
    Pop-Location
  }

  $apiUrl = Invoke-AwsText @("cloudformation", "describe-stacks", "--stack-name", $stackName, "--query", "Stacks[0].Outputs[?OutputKey=='ApiUrl'].OutputValue | [0]")
  $lambdaName = Invoke-AwsText @("cloudformation", "describe-stacks", "--stack-name", $stackName, "--query", "Stacks[0].Outputs[?OutputKey=='FunctionName'].OutputValue | [0]")
  $httpApiId = Invoke-AwsText @("cloudformation", "describe-stacks", "--stack-name", $stackName, "--query", "Stacks[0].Outputs[?OutputKey=='HttpApiId'].OutputValue | [0]")
}

$apiHost = ([uri]$apiUrl).Host
$apiPath = "/v1"

$instanceId = ""
$publicIp = ""
if (-not $SkipEc2) {
  Write-Section "IAM + EC2"
  Ensure-IamRoleAndProfile -RoleName $roleName -ProfileName $profileName -PolicyName $policyName -PolicyFile $PolicyPath
  $vpcId = if ($DryRun) { "vpc-dryrun" } else { Invoke-AwsText @("ec2", "describe-vpcs", "--filters", "Name=isDefault,Values=true", "--query", "Vpcs[0].VpcId") }
  if (-not $vpcId -or $vpcId -eq "None") { throw "No default VPC found in $Region" }
  $sgId = Ensure-SecurityGroup -GroupName $sgName -VpcId $vpcId -Cidr $allowCidr

  New-UserDataFile `
    -TemplatePath $TemplatePath `
    -OutPath $UserDataOut `
    -DashboardAppPath $DashboardAppPath `
    -RequirementsPath $DashboardReqPath `
    -AwsRegion $Region `
    -TsDb $dbName `
    -TsTele $teleTable `
    -TsEvt $evtTable `
    -AppUser $StreamlitUser `
    -AppHash $StreamlitPasswordSha256

  $instanceId = Ensure-EC2Instance `
    -NameTag $ec2Name `
    -KeyPair $KeyPairName `
    -SgId $sgId `
    -InstanceProfile $profileName `
    -UserDataFile $UserDataOut `
    -InstanceType "t3.micro" `
    -ForceRecreate:$ForceRecreateEc2

  if (-not $DryRun) {
    $publicIp = Invoke-AwsText @("ec2", "describe-instances", "--instance-ids", $instanceId, "--query", "Reservations[0].Instances[0].PublicIpAddress")
  } else {
    $publicIp = "0.0.0.0"
  }
}

Write-Section "Health checks"
$apiHealth = Test-ApiEndpoint -ApiUrl $apiUrl -ApiToken $ApiToken -DeviceId $DeviceId
if ($apiHealth.ok) {
  Write-Host "API health OK (HTTP $($apiHealth.status))" -ForegroundColor Green
} else {
  Write-Host "API health FAILED: $($apiHealth.err)" -ForegroundColor Red
}

Write-Section "Firmware CFG commands"
Write-Host "Use USB serial @ 115200 and send:" -ForegroundColor Cyan
Write-Host "CFG WIFI_SSID $WifiSsid"
if ([string]::IsNullOrWhiteSpace($WifiPass)) {
  Write-Host "CFG WIFI_PASS"
} else {
  Write-Host "CFG WIFI_PASS $WifiPass"
}
Write-Host "CFG API_HOST $apiHost"
Write-Host "CFG API_PATH $apiPath"
Write-Host "CFG API_TOKEN $ApiToken"
Write-Host "CFG DEVICE_ID $DeviceId"
Write-Host "CFG WIFI_ENABLE 1"
Write-Host "CFG SAVE"
Write-Host "CFG TEST"
Write-Host "CFG SHOW"

Write-Section "Summary"
Write-Host "Region: $Region"
Write-Host "Stack: $stackName"
Write-Host "Lambda: $lambdaName"
Write-Host "HttpApiId: $httpApiId"
Write-Host "ApiUrl: $apiUrl"
if (-not $SkipEc2) {
  Write-Host "EC2 instance: $instanceId"
  Write-Host "Dashboard URL: http://$publicIp:8501"
  Write-Host "Dashboard login user: $StreamlitUser"
}

$receipt = [ordered]@{
  timestamp_utc = (Get-Date).ToUniversalTime().ToString("o")
  region = $Region
  project_prefix = $ProjectPrefix
  stack_name = $stackName
  lambda_name = $lambdaName
  http_api_id = $httpApiId
  api_url = $apiUrl
  api_host = $apiHost
  api_path = $apiPath
  timestream_db = $dbName
  timestream_table_telemetry = $teleTable
  timestream_table_events = $evtTable
  ec2_instance_id = $instanceId
  ec2_public_ip = $publicIp
  streamlit_user = $StreamlitUser
  allowed_cidr = $allowCidr
  dry_run = [bool]$DryRun
}

$outDir = Join-Path $ScriptRoot "out"
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Force $outDir | Out-Null }
$receiptPath = Join-Path $outDir ("$ProjectPrefix-deployment.json")
$receipt | ConvertTo-Json -Depth 6 | Set-Content $receiptPath
Write-Host "Receipt saved: $receiptPath" -ForegroundColor Green
