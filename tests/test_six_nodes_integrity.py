import os
import unittest

class TestSixNodesIntegrity(unittest.TestCase):
    def setUp(self):
        self.repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        self.firmware_dir = os.path.join(self.repo_root, "firmware")
        self.nodes = [f"ARVANODE-00{i}" for i in range(1, 7)]

    def test_all_six_node_directories_exist(self):
        for node in self.nodes:
            ndir = os.path.join(self.firmware_dir, node)
            self.assertTrue(os.path.isdir(ndir), f"Directory missing for {node}")

    def test_all_six_node_sketches_and_headers_exist(self):
        for node in self.nodes:
            ndir = os.path.join(self.firmware_dir, node)
            ino_file = os.path.join(ndir, f"{node}.ino")
            config_file = os.path.join(ndir, "node_config.h")
            self.assertTrue(os.path.isfile(ino_file), f"Sketch missing: {ino_file}")
            self.assertTrue(os.path.isfile(config_file), f"Config missing: {config_file}")

    def test_unique_node_identities_and_keys(self):
        keys = set()
        node_ids = set()
        for node in self.nodes:
            config_file = os.path.join(self.firmware_dir, node, "node_config.h")
            with open(config_file, "r") as f:
                content = f.read()
                
            # Check NODE_LOGICAL_ID
            self.assertIn(f'#define NODE_LOGICAL_ID "{node}"', content)
            node_ids.add(node)
            
            # Extract key
            for line in content.splitlines():
                if "NODE_DEVICE_KEY" in line:
                    key = line.split('"')[1]
                    self.assertTrue(key.startswith("dev_sec_"), f"Invalid key prefix in {node}")
                    keys.add(key)
                    
        self.assertEqual(len(node_ids), 6, "Expected 6 unique node IDs")
        self.assertEqual(len(keys), 6, "Expected 6 unique cryptographic device keys")

    def test_lora_specification_exists(self):
        lora_doc = os.path.join(self.firmware_dir, "ARVA_LORA_PROTOCOL_AND_TDMA.md")
        self.assertTrue(os.path.isfile(lora_doc), "LoRa protocol specification missing")
        with open(lora_doc, "r") as f:
            content = f.read()
            self.assertIn("433.00 MHz", content)
            self.assertIn("TDMA Transmission Schedule", content)

if __name__ == "__main__":
    unittest.main()
