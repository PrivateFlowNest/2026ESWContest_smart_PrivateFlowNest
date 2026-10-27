# Render the report with the installed PowerPoint: .pptx -> .pdf (fonts embedded) for submission and QA.
#   powershell -ExecutionPolicy Bypass -File render.ps1 <deck.pptx>
param([Parameter(Mandatory = $true)][string]$Deck)
$deck = (Resolve-Path $Deck).Path
$pdf = [System.IO.Path]::ChangeExtension($deck, ".pdf")
$app = New-Object -ComObject PowerPoint.Application
try {
  $presentation = $app.Presentations.Open($deck, $true, $false, $false)  # read-only, no window
  $presentation.SaveAs($pdf, 32)  # ppSaveAsPDF
  $presentation.Close()
  Write-Output $pdf
} finally {
  $app.Quit()
}
