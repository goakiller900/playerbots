"""Source/deployment guards, not database atomicity or server integration tests."""
from pathlib import Path
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SafetyContract(unittest.TestCase):
    def test_auction_coordinator_is_scoped_and_hard_gated(self):
        core = ROOT / ".core-reference"
        if not core.exists():
            self.skipTest("local pinned reference not present")
        handler = (core / "src/game/AuctionHouse/AuctionHouseHandler.cpp").read_text()
        manager = (ROOT / "playerbot/RandomBotEstateAuctionMgr.cpp").read_text()
        gate = (ROOT / "playerbot/RandomBotLifecycle.h").read_text()
        self.assertIn("sRandomBotEstateAuctionMgr.HandleBid", handler)
        self.assertIn("if (!session || !auction || !IsTracked(auction->Id))", manager)
        self.assertIn("RandomBotLifecycleMgr::ExecutionAllowed()", manager)
        self.assertIn("static bool ExecutionAllowed() { return false; }", gate)

    def test_retirement_scheduler_still_hard_disabled(self):
        source = (ROOT / "playerbot/RandomBotLifecycle.cpp").read_text()
        update = source.split("void RandomBotLifecycleMgr::Update(", 1)[1]
        self.assertIn("No retirements or economic mutations will be performed.", update)
        gate = update.index("if (!ExecutionAllowed())")
        scheduler = update.index("ScheduleWork();")
        self.assertLess(gate, scheduler)
        self.assertIn("return;", update[gate:scheduler])
        header = (ROOT / "playerbot/RandomBotLifecycle.h").read_text()
        self.assertIn("static bool ExecutionAllowed() { return false; }", header)
        core = ROOT / ".core-reference/src/game/AuctionHouse"
        if core.exists():
            for name in ("AuctionHouseMgr.cpp", "AuctionHouseMgr.h", "AuctionHouseHandler.cpp"):
                self.assertNotIn("retirementSettlementPending", (core / name).read_text())

    def test_configuration_is_opt_in(self):
        for suffix in ("", ".tbc", ".wotlk"):
            text = (ROOT / ("playerbot/aiplayerbot.conf.dist.in" + suffix)).read_text()
            for option in ("Retirement.Enabled", "NaturalLeveling.Enabled",
                           "LevelBracketBalancing.Enabled", "LevelBracketInitialRandomization.Enabled"):
                self.assertRegex(text, rf"(?m)^AiPlayerbot\.{re.escape(option)}\s*=\s*0\s*$")
            self.assertRegex(text, r"(?m)^AiPlayerbot\.Retirement.CharacterDisposition\s*=\s*archive\s*$")
            for faction in ("Alliance", "Horde"):
                for field in ("Guid", "Account"):
                    self.assertRegex(text, rf"(?m)^AiPlayerbot\.Retirement\.Broker\.{faction}\.{field}\s*=\s*0\s*$")
            for faction in ("AllianceBidder", "HordeBidder"):
                for field in ("Guid", "Account"):
                    self.assertRegex(text, rf"(?m)^AiPlayerbot\.Retirement\.Broker\.{faction}\.{field}\s*=\s*0\s*$")

    def test_broker_schema_is_additive_and_survives_character_deletion(self):
        sql = (ROOT / "sql/characters/ai_playerbot_estate.sql").read_text()
        sql = re.sub(r"--[^\n]*", "", sql)
        statements = [statement.strip() for statement in sql.split(";") if statement.strip()]
        self.assertEqual(len(statements), 8)
        for statement in statements:
            self.assertTrue(statement.startswith("CREATE TABLE IF NOT EXISTS `ai_playerbot_estate"))
            self.assertIn("ENGINE=InnoDB", statement)
            self.assertNotIn("REFERENCES", statement)
        self.assertIn("`work_queue` (`status`,`next_action_at`", sql)

    def test_broker_guard_boundaries(self):
        manager = (ROOT / "playerbot/RandomBotLifecycle.cpp").read_text()
        self.assertIn("sRandomBotEstateService.IsServiceCharacter(guid)", manager)
        store = (ROOT / "playerbot/RandomBotLifecycleStore.cpp").read_text()
        self.assertEqual(store.count("sRandomBotEstateService.IsServiceAccount(account)"), 2)
        factory = (ROOT / "playerbot/RandomPlayerbotFactory.cpp").read_text()
        self.assertLess(factory.index("sRandomBotEstateService.HasServiceAccounts()"),
                        factory.index("// check if scheduled for delete"))
        core = ROOT / ".core-reference"
        if core.exists():
            account = (core / "src/game/Accounts/AccountMgr.cpp").read_text()
            deletion = account.split("AccountOpResult AccountMgr::DeleteAccount(", 1)[1]
            self.assertLess(deletion.index("sRandomBotEstateService.IsServiceAccount"),
                            deletion.index("DELETE FROM"))

    def test_migration_only_creates_optional_tables(self):
        sql = (ROOT / "sql/characters/ai_playerbot_lifecycle.sql").read_text()
        sql = re.sub(r"--[^\n]*", "", sql)
        statements = [statement.strip() for statement in sql.split(";") if statement.strip()]
        self.assertEqual(len(statements), 6)
        for statement in statements:
            self.assertTrue(statement.startswith("CREATE TABLE IF NOT EXISTS `ai_playerbot_lifecycle"))
            self.assertIn("ENGINE=InnoDB", statement)

    def test_worker_accounting_does_not_use_query_pool(self):
        for name in ("RandomBotLifecycleStore.cpp", "RandomBotLifecycleAssets.cpp", "RandomBotLifecycleAuctions.cpp",
                     "RandomBotLifecycleAuctionReturns.cpp", "RandomBotEstateStore.cpp", "RandomBotEstateLiquidation.cpp"):
            source = (ROOT / "playerbot" / name).read_text()
            self.assertNotRegex(source, r"CharacterDatabase\.(PQuery|Query|PExecute|Execute)\(")
            self.assertIn("connection.Query", source)
            self.assertIn("connection.Execute", source)
        assets = (ROOT / "playerbot/RandomBotLifecycleAssets.cpp").read_text()
        self.assertIn("CMANGOS_ASYNC_TRANSACTION_CALLBACK < 2", assets)
        self.assertIn("CommitTransactionAcknowledged", assets)

    def test_exported_patch_matches_pinned_reference(self):
        core = ROOT / ".core-reference"
        if not core.exists():
            self.skipTest("local pinned reference not present")
        head = subprocess.check_output(["git", "-C", str(core), "rev-parse", "HEAD"], text=True).strip()
        self.assertEqual(head, "1cd9d566ae83c1a88f1b057514697055055f419c")
        subprocess.run(["git", "-C", str(core), "apply", "--reverse", "--check",
                        str(ROOT / "core-patches/0001-lifecycle-transaction-and-login-foundation.patch")],
                       check=True)


if __name__ == "__main__":
    unittest.main()
