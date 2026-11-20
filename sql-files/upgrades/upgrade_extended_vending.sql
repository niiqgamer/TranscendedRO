-- Extended Vending: currency item of a vending shop (0 = Zeny)
ALTER TABLE `vendings` ADD COLUMN `extended_vending_item` int(10) unsigned NOT NULL DEFAULT '0' AFTER `autotrade`;
