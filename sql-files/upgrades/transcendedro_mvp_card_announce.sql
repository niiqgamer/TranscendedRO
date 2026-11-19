-- Transcended RO - MVP Card Announce queue
-- Written by the map-server (src/map/mob.cpp, mob_queue_mvp_card_announce)
-- Relayed to Discord by tools/discord_mvp_card.php

CREATE TABLE IF NOT EXISTS `mvp_card_announce` (
  `id` int(11) unsigned NOT NULL AUTO_INCREMENT,
  `drop_time` datetime NOT NULL,
  `nameid` int(10) unsigned NOT NULL,
  `item_name` varchar(100) NOT NULL DEFAULT '',
  `mob_id` int(11) unsigned NOT NULL DEFAULT '0',
  `mob_name` varbinary(50) NOT NULL DEFAULT '',
  `drop_rate` int(11) unsigned NOT NULL DEFAULT '0',
  `char_id` int(11) unsigned NOT NULL DEFAULT '0',
  `char_name` varbinary(30) NOT NULL DEFAULT '',
  `map` varchar(16) NOT NULL DEFAULT '',
  `x` smallint(5) unsigned NOT NULL DEFAULT '0',
  `y` smallint(5) unsigned NOT NULL DEFAULT '0',
  `discord_sent` tinyint(1) unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`id`),
  KEY `discord_sent` (`discord_sent`)
) ENGINE=MyISAM;
