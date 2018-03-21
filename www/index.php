<?php
require '../Credis/Client.php';
$redis = new Credis_Client('localhost');
$redis->set('awesome', 'hahahahaha');
echo sprintf('Is Credis awesome? %s.\n', $redis->get('awesome'));

// When arrays are given as arguments they are flattened automatically
$redis->rpush('particles', array('proton','electron','neutron'));
$particles = $redis->lrange('particles', 0, -1);

?>
