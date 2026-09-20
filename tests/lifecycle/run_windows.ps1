param([string]$Compiler = ".validation-tools/ziglang/zig.exe")
$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
Push-Location $repoRoot
try {
    $compilerPath = (Resolve-Path -LiteralPath $Compiler).Path
    $tests = @(
        @{ Name = "lifecycle_math"; Flags = @("-DPLAYERBOT_LIFECYCLE_STANDALONE_TEST", "-I."); Sources = @("playerbot/RandomBotLifecycleMath.cpp", "playerbot/strategy/tests/RandomBotLifecycleMathTest.cpp", "tests/lifecycle/main.cpp") },
        @{ Name = "transaction_ack"; Flags = @("-I.core-reference/src/shared"); Sources = @("tests/lifecycle/transaction_ack.cpp") },
        @{ Name = "estate_intake"; Flags = @("-Itests/lifecycle/stubs", "-Iplayerbot", "-I.core-reference/src/shared"); Sources = @("tests/lifecycle/estate_intake.cpp", "playerbot/RandomBotEstate.cpp", "playerbot/RandomBotEstateStore.cpp") },
        @{ Name = "estate_liquidation"; Flags = @("-Itests/lifecycle/stubs", "-Iplayerbot", "-I.core-reference/src/shared"); Sources = @("tests/lifecycle/estate_liquidation.cpp", "playerbot/RandomBotEstateLiquidation.cpp") },
        @{ Name = "estate_auction_listing"; Flags = @("-Itests/lifecycle/stubs", "-Iplayerbot", "-I.core-reference/src/shared"); Sources = @("tests/lifecycle/estate_auction_listing.cpp", "playerbot/RandomBotEstateAuctions.cpp") },
        @{ Name = "estate_auction_bid"; Flags = @("-Itests/lifecycle/stubs", "-Iplayerbot", "-I.core-reference/src/shared"); Sources = @("tests/lifecycle/estate_auction_bid.cpp", "playerbot/RandomBotEstateAuctionBids.cpp") },
        @{ Name = "estate_auction_settlement"; Flags = @("-Itests/lifecycle/stubs", "-Iplayerbot", "-I.core-reference/src/shared"); Sources = @("tests/lifecycle/estate_auction_settlement.cpp", "playerbot/RandomBotEstateAuctionSettlement.cpp") },
        @{ Name = "asset_save"; Flags = @("-Itests/lifecycle/stubs", "-Iplayerbot", "-I.core-reference/src/shared"); Sources = @("tests/lifecycle/asset_save.cpp", "playerbot/RandomBotLifecycleAssets.cpp") },
        @{ Name = "auction_payout"; Flags = @("-Itests/lifecycle/stubs", "-Iplayerbot", "-I.core-reference/src/shared"); Sources = @("tests/lifecycle/auction_payout.cpp", "playerbot/RandomBotLifecycleAuctions.cpp") },
        @{ Name = "auction_return"; Flags = @("-Itests/lifecycle/stubs", "-Iplayerbot", "-I.core-reference/src/shared"); Sources = @("tests/lifecycle/auction_return.cpp", "playerbot/RandomBotLifecycleAuctionReturns.cpp") }
    )
    foreach ($test in $tests) {
        $outputPath = Join-Path $repoRoot (".validation-tools/" + $test.Name + ".exe")
        $compilerArgs = @("c++", "-std=c++14", "-Wall", "-Wextra", "-Werror", "-UNDEBUG") + $test.Flags + $test.Sources + @("-o", $outputPath)
        & $compilerPath @compilerArgs
        if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $($test.Name)" }
        & $outputPath
        if ($LASTEXITCODE -ne 0) { throw "Test failed: $($test.Name)" }
        Write-Output ("PASS " + $test.Name)
    }
    python tests/lifecycle/test_safety_contract.py -v
    if ($LASTEXITCODE -ne 0) { throw "Safety contract tests failed" }
    git diff --check
    if ($LASTEXITCODE -ne 0) { throw "PlayerBots diff check failed" }
    git -C .core-reference diff --check
    if ($LASTEXITCODE -ne 0) { throw "Core diff check failed" }
} finally {
    Pop-Location
}
