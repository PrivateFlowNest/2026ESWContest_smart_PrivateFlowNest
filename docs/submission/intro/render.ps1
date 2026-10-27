# Render a .docx with the installed Word: .docx -> .pdf (fonts embedded) for submission and QA.
#   powershell -ExecutionPolicy Bypass -File render.ps1 <file.docx>
param([Parameter(Mandatory = $true)][string]$Document)
$doc = (Resolve-Path $Document).Path
$pdf = [System.IO.Path]::ChangeExtension($doc, ".pdf")
$app = New-Object -ComObject Word.Application
$app.Visible = $false
try {
  $file = $app.Documents.Open($doc, $false, $true)  # no conversion prompt, read-only
  $file.ExportAsFixedFormat($pdf, 17)  # wdExportFormatPDF
  Write-Output "pages=$($file.ComputeStatistics(2))"  # wdStatisticPages
  $file.Close($false)
  Write-Output $pdf
} finally {
  $app.Quit()
}
