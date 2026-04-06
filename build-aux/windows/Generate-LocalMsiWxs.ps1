param(
    [Parameter(Mandatory = $true)]
    [string]$BundleRoot,

    [Parameter(Mandatory = $true)]
    [string]$OutputFile
)

$ErrorActionPreference = "Stop"

function New-WixId {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Prefix,

        [Parameter(Mandatory = $true)]
        [string]$InputText
    )

    $hash = [System.Security.Cryptography.SHA1]::HashData([System.Text.Encoding]::UTF8.GetBytes($InputText))
    $hex = ([System.BitConverter]::ToString($hash)).Replace("-", "")
    return "{0}_{1}" -f $Prefix, $hex.Substring(0, 16)
}

function New-StableGuid {
    param(
        [Parameter(Mandatory = $true)]
        [string]$InputText
    )

    $hash = [System.Security.Cryptography.SHA1]::HashData([System.Text.Encoding]::UTF8.GetBytes($InputText))
    $bytes = New-Object byte[] 16
    [Array]::Copy($hash, $bytes, 16)

    # Set RFC 4122 version 5 / variant bits so the GUID is deterministic and valid.
    $bytes[6] = ($bytes[6] -band 0x0F) -bor 0x50
    $bytes[8] = ($bytes[8] -band 0x3F) -bor 0x80

    return [guid]::New($bytes).ToString().ToUpperInvariant()
}

function Escape-Wix {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Text
    )

    return [System.Security.SecurityElement]::Escape($Text)
}

function Get-RelativePathCompat {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BasePath,

        [Parameter(Mandatory = $true)]
        [string]$TargetPath
    )

    if ([System.IO.Path].GetMethod("GetRelativePath", [Type[]]@([string], [string])) -ne $null) {
        return [System.IO.Path]::GetRelativePath($BasePath, $TargetPath)
    }

    $baseUri = [System.Uri]((Resolve-Path -LiteralPath $BasePath).Path.TrimEnd('\') + '\')
    $targetUri = [System.Uri](Resolve-Path -LiteralPath $TargetPath).Path
    $relativeUri = $baseUri.MakeRelativeUri($targetUri)
    return [System.Uri]::UnescapeDataString($relativeUri.ToString()).Replace('/', '\')
}

$bundleRoot = [System.IO.Path]::GetFullPath($BundleRoot)
$outputPath = [System.IO.Path]::GetFullPath($OutputFile)
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$iconPath = Join-Path $repoRoot "resources\windows\smashed-pumpkin.ico"

if (-not (Test-Path -LiteralPath $bundleRoot -PathType Container)) {
    throw "Bundle root not found: $bundleRoot"
}

$directories = Get-ChildItem -LiteralPath $bundleRoot -Directory -Recurse |
    Sort-Object FullName
$files = Get-ChildItem -LiteralPath $bundleRoot -File -Recurse |
    Sort-Object FullName

$dirIds = @{}
$dirIds["."] = "INSTALLDIR"

foreach ($dir in $directories) {
    $relative = Get-RelativePathCompat -BasePath $bundleRoot -TargetPath $dir.FullName
    $dirIds[$relative] = New-WixId -Prefix "DIR" -InputText $relative
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('<?xml version="1.0" encoding="UTF-8"?>')
$lines.Add('<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs">')
$lines.Add('  <Package')
$lines.Add('    Name="Smashed Pumpkin"')
$lines.Add('    Manufacturer="Rotstein"')
$lines.Add('    Language="1033"')
$lines.Add('    Version="0.5.1"')
$lines.Add('    UpgradeCode="8E643E21-5F9C-4D1C-89F1-A6B72E01F620"')
$lines.Add('    Scope="perMachine">')
$lines.Add('    <SummaryInformation')
$lines.Add('      Description="Desktop management app for Pumpkin servers"')
$lines.Add('      Manufacturer="Rotstein" />')
$lines.Add('    <MajorUpgrade DowngradeErrorMessage="A newer version of [ProductName] is already installed." />')
$lines.Add('    <MediaTemplate EmbedCab="yes" />')
$lines.Add(("    <Icon Id=""ProductIcon"" SourceFile=""{0}"" />" -f (Escape-Wix $iconPath)))
$lines.Add('    <Property Id="ARPPRODUCTICON" Value="ProductIcon" />')
$lines.Add('    <Property Id="ARPURLINFOABOUT" Value="https://github.com/Rotstein007/smashed-pumpkin" />')
$lines.Add('    <Property Id="ARPHELPLINK" Value="https://github.com/Rotstein007/smashed-pumpkin/issues" />')
$lines.Add('    <Property Id="ARPCONTACT" Value="Rotstein" />')
$lines.Add('')
$lines.Add('    <StandardDirectory Id="ProgramFiles64Folder">')
$lines.Add('      <Directory Id="INSTALLROOT" Name="Smashed Pumpkin">')
$lines.Add('        <Directory Id="INSTALLDIR" Name="app">')

$childrenByParent = @{}
foreach ($dir in $directories) {
    $relative = Get-RelativePathCompat -BasePath $bundleRoot -TargetPath $dir.FullName
    $parentRelative = [System.IO.Path]::GetDirectoryName($relative)
    if ([string]::IsNullOrEmpty($parentRelative)) {
        $parentRelative = "."
    }

    if (-not $childrenByParent.ContainsKey($parentRelative)) {
        $childrenByParent[$parentRelative] = New-Object System.Collections.Generic.List[string]
    }

    $childrenByParent[$parentRelative].Add($relative)
}

function Add-DirectoryTree {
    param(
        [string]$ParentRelative,
        [int]$Indent
    )

    if (-not $childrenByParent.ContainsKey($ParentRelative)) {
        return
    }

    foreach ($childRelative in ($childrenByParent[$ParentRelative] | Sort-Object)) {
        $dirName = Split-Path -Leaf $childRelative
        $dirId = $dirIds[$childRelative]
        $prefix = (' ' * $Indent)
        $lines.Add(('{0}<Directory Id="{1}" Name="{2}">' -f $prefix, $dirId, (Escape-Wix $dirName)))
        Add-DirectoryTree -ParentRelative $childRelative -Indent ($Indent + 2)
        $lines.Add(('{0}</Directory>' -f $prefix))
    }
}

Add-DirectoryTree -ParentRelative "." -Indent 10

$lines.Add('        </Directory>')
$lines.Add('      </Directory>')
$lines.Add('    </StandardDirectory>')
$lines.Add('')
$lines.Add('    <StandardDirectory Id="ProgramMenuFolder">')
$lines.Add('      <Directory Id="ProgramMenuDir" Name="Smashed Pumpkin" />')
$lines.Add('    </StandardDirectory>')
$lines.Add('')
$lines.Add('    <Feature Id="MainFeature" Title="Smashed Pumpkin" Level="1">')
$lines.Add('      <ComponentGroupRef Id="AppFiles" />')
$lines.Add('      <ComponentRef Id="StartMenuShortcutComponent" />')
$lines.Add('    </Feature>')
$lines.Add('  </Package>')
$lines.Add('')
$lines.Add('  <Fragment>')
$lines.Add('    <ComponentGroup Id="AppFiles">')

foreach ($file in $files) {
    $relative = Get-RelativePathCompat -BasePath $bundleRoot -TargetPath $file.FullName
    $parentRelative = [System.IO.Path]::GetDirectoryName($relative)
    if ([string]::IsNullOrEmpty($parentRelative)) {
        $parentRelative = "."
    }

    $dirId = $dirIds[$parentRelative]
    $componentId = New-WixId -Prefix "CMP" -InputText $relative
    $fileId = New-WixId -Prefix "FIL" -InputText $relative
    $guid = New-StableGuid -InputText ("component:" + $relative)
    $source = Escape-Wix($file.FullName)

    $lines.Add(("      <Component Id=""{0}"" Directory=""{1}"" Guid=""{2}"">" -f $componentId, $dirId, $guid))
    $lines.Add(("        <File Id=""{0}"" Source=""{1}"" KeyPath=""yes"" />" -f $fileId, $source))
    $lines.Add('      </Component>')
}

$lines.Add('    </ComponentGroup>')
$lines.Add('  </Fragment>')
$lines.Add('')
$lines.Add('  <Fragment>')
$lines.Add('    <DirectoryRef Id="ProgramMenuDir">')
$lines.Add('      <Component Id="StartMenuShortcutComponent" Guid="A02EBB2D-B6B4-4FD9-9E88-96DBE60E1226">')
$lines.Add('        <Shortcut')
$lines.Add('          Id="ApplicationStartMenuShortcut"')
$lines.Add('          Name="Smashed Pumpkin"')
$lines.Add('          Description="Desktop management app for Pumpkin servers"')
$lines.Add('          Target="[INSTALLDIR]bin\smashed-pumpkin.exe"')
$lines.Add('          WorkingDirectory="INSTALLDIR"')
$lines.Add('          Icon="ProductIcon"')
$lines.Add('          IconIndex="0" />')
$lines.Add('        <RemoveFolder Id="RemoveProgramMenuDir" On="uninstall" />')
$lines.Add('        <RegistryValue')
$lines.Add('          Root="HKLM"')
$lines.Add('          Key="Software\Rotstein\Smashed Pumpkin"')
$lines.Add('          Name="Installed"')
$lines.Add('          Type="integer"')
$lines.Add('          Value="1"')
$lines.Add('          KeyPath="yes" />')
$lines.Add('      </Component>')
$lines.Add('    </DirectoryRef>')
$lines.Add('  </Fragment>')
$lines.Add('</Wix>')

[System.IO.File]::WriteAllLines($outputPath, $lines)
