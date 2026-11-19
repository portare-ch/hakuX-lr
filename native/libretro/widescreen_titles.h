/*
 * hakuX-lr - the titles that display a 16:9 picture when the console is set to
 * widescreen, by title ID (the XBE certificate's), sorted.
 *
 * From Wikipedia's "List of Xbox games with alternate display modes" (its
 * table of native support, as of 2018-08-17; text under CC BY-SA), matched by
 * name to the North American releases. A title the list doesn't have, or has
 * as 4:3, is not here. Left out against the list: Star Wars - Republic
 * Commando (4C410013: no widescreen together with 480p, which the console
 * here is set to), Dead or Alive Ultimate (54430006: one ID for two discs,
 * the first of them 4:3) and Battlestar Galactica (5655000F: listed both
 * ways).
 */
#ifndef NC_LR_WIDESCREEN_TITLES_H
#define NC_LR_WIDESCREEN_TITLES_H

#include <stdint.h>

static const uint32_t k_widescreen_titles[] = {
    0x41430006U, /* Burnout */
    0x41430007U, /* Aggressive Inline */
    0x4143000AU, /* ATV - Quad Power Racing 2 */
    0x4143000CU, /* Freestyle Street Soccer */
    0x4143000EU, /* BMX XXX */
    0x41430017U, /* Dakar 2 - The World's Ultimate Rally */
    0x41430019U, /* Burnout 2 - Point of Impact */
    0x4143001AU, /* Gladiator - Sword of Vengeance */
    0x4143001DU, /* Showdown - Legends of Wrestling */
    0x41540004U, /* Galleon */
    0x41560003U, /* Street Hoops */
    0x41560005U, /* Blade II */
    0x41560007U, /* X-Men - Next Dimension */
    0x41560008U, /* X2 - Wolverine's Revenge */
    0x4156000BU, /* Mat Hoffman's Pro BMX 2 */
    0x4156000EU, /* True Crime - Streets of LA */
    0x41560017U, /* Tony Hawk's Pro Skater 4 */
    0x4156001AU, /* Fantastic 4 */
    0x4156001BU, /* Soldier of Fortune II - Double Helix */
    0x4156001EU, /* X-Men Legends */
    0x41560020U, /* Doom 3 */
    0x41560027U, /* Disney's Extreme Skate Adventure */
    0x4156002DU, /* Tony Hawk's Underground */
    0x41560030U, /* Cabela's Deer Hunt - 2004 Season */
    0x4156003CU, /* Tony Hawk's Underground 2 */
    0x4156003EU, /* Cabela's Big Game Hunter 2005 Adventures */
    0x4156003FU, /* Cabela's Deer Hunt - 2005 Season */
    0x41560040U, /* Bicycle Casino Includes - Texas Hold'em */
    0x41560045U, /* True Crime - New York City */
    0x41560047U, /* X-Men Legends II - Rise of Apocalypse */
    0x41560049U, /* Tony Hawk's American Wasteland */
    0x4156004BU, /* Gun */
    0x4156004DU, /* Cabela's Outdoor Adventures */
    0x4156004EU, /* Doom 3 - Resurrection of Evil */
    0x4156004FU, /* American Chopper 2 - Full Throttle */
    0x41560051U, /* Call of Duty 2 - Big Red One */
    0x41560052U, /* Cabela's Dangerous Hunts 2 */
    0x41560055U, /* TY the Tasmanian Tiger - Night of the Quinkan */
    0x4156005CU, /* Marvel - Ultimate Alliance */
    0x4156005DU, /* Call of Duty 3 */
    0x4156005EU, /* Tony Hawk's Project 8 */
    0x41590004U, /* Dreamfall - The Longest Journey */
    0x42410001U, /* Digimon Rumble Arena 2 */
    0x42410002U, /* Digimon World 4 */
    0x424D0003U, /* Wallace & Gromit in Project Zoo */
    0x42530004U, /* Pirates of the Caribbean */
    0x4253000BU, /* Power Drome */
    0x42550004U, /* Double-S T E A L - The Second Clash */
    0x42560004U, /* Tim Burton's The Nightmare Before Christmas - Oogie's Revenge */
    0x43430002U, /* Steel Battalion */
    0x43430009U, /* Steel Battalion - Line of Contact */
    0x434D0001U, /* Colin McRae Rally 3 */
    0x434D0003U, /* Mike Tyson Heavyweight Boxing */
    0x434D0010U, /* Colin McRae Rally 04 */
    0x434D0011U, /* TOCA Race Driver 2 */
    0x434D002BU, /* Colin McRae Rally 2005 */
    0x43560007U, /* Future Tactics - The Uprising */
    0x43560009U, /* Trigger Man */
    0x4356000AU, /* Pinball Hall of Fame - The Gottlieb Collection */
    0x43560011U, /* Bible Game, The */
    0x45410002U, /* NASCAR Thunder 2002 */
    0x45410004U, /* SSX Tricky */
    0x45410007U, /* F1 2001 */
    0x45410015U, /* Pirates - The Legend of Black Kat */
    0x45410026U, /* 007 - Nightfire */
    0x45410029U, /* TY the Tasmanian Tiger */
    0x4541002EU, /* Freedom Fighters */
    0x4541003CU, /* SSX 3 */
    0x4541003DU, /* Harry Potter - Quidditch World Cup */
    0x45410042U, /* 007 - Everything or Nothing */
    0x45410043U, /* Sims, The - Bustin' Out */
    0x45410045U, /* MVP Baseball 2004 */
    0x4541004BU, /* Fight Night 2004 */
    0x45410053U, /* NASCAR 2005 - Chase for the Cup */
    0x45410055U, /* Fight Night Round 2 */
    0x45410056U, /* MVP Baseball 2005 */
    0x4541005AU, /* Need for Speed - Underground 2 */
    0x4541005BU, /* Burnout 3 - Takedown */
    0x4541005CU, /* Catwoman */
    0x45410060U, /* Urbz, The - Sims in the City */
    0x45410063U, /* TY the Tasmanian Tiger 2 - Bush Rescue */
    0x45410066U, /* TimeSplitters - Future Perfect */
    0x45410072U, /* FIFA Street */
    0x45410076U, /* Burnout Revenge */
    0x45410077U, /* Harry Potter and the Goblet of Fire */
    0x45410078U, /* FIFA Soccer 06 */
    0x4541007AU, /* NBA Live 06 */
    0x4541007BU, /* Need for Speed - Most Wanted / Need for Speed - Most Wanted - Black Edition */
    0x4541007DU, /* Godfather, The */
    0x45410082U, /* SSX On Tour */
    0x45410083U, /* Black */
    0x45410085U, /* FIFA Street 2 */
    0x45410086U, /* Fight Night Round 3 */
    0x45410087U, /* Rugby 06 */
    0x4541008DU, /* MVP 06 NCAA Baseball */
    0x45410091U, /* Half-Life 2 */
    0x45410094U, /* Arena Football */
    0x4541009CU, /* FIFA World Cup Germany 2006 */
    0x454100A0U, /* Superman Returns */
    0x454100A1U, /* NBA Live 07 */
    0x454100A6U, /* FIFA Soccer 07 */
    0x45410389U, /* Batman Begins */
    0x4541038AU, /* Marvel Nemesis - Rise of the Imperfects */
    0x4541038BU, /* Sims 2, The */
    0x45430004U, /* Heroes of the Pacific */
    0x454D0005U, /* Starsky & Hutch */
    0x454D0007U, /* Ford Racing 2 */
    0x454D000DU, /* Ford Racing 3 */
    0x454D0017U, /* Big Mutha Truckers 2 */
    0x454D001FU, /* Ford Street Racing */
    0x454D0020U, /* FlatOut 2 */
    0x45530008U, /* Commandos 2 - Men of Courage */
    0x4553000FU, /* Legacy of Kain - Defiance */
    0x45530018U, /* 25 to Life */
    0x47560003U, /* Combat - Task Force 121 */
    0x47560009U, /* World War II Combat - Road to Berlin */
    0x4756000BU, /* World War II Combat - Iwo Jima */
    0x4947000BU, /* Furious Karting */
    0x4947000FU, /* Monopoly Party */
    0x49470016U, /* Superman - The Man of Steel */
    0x49470022U, /* Enter the Matrix */
    0x49470026U, /* Terminator, The - Dawn of Fate */
    0x49470038U, /* Driv3r */
    0x4947003BU, /* Terminator 3 - Rise of the Machines */
    0x4947003EU, /* Terminator 3 - The Redemption */
    0x49470075U, /* Test Drive - Eve of Destruction */
    0x49470079U, /* Atari Anthology */
    0x4947007AU, /* Dragon Ball Z - Sagas */
    0x4947007BU, /* Indigo Prophecy */
    0x4947007FU, /* Driver - Parallel Lines */
    0x49500006U, /* RLH - Run Like Hell */
    0x4B420002U, /* Egg Mania - Eggstreme Madness */
    0x4B4E0009U, /* Winter X Games Snowboarding 2002 */
    0x4B4E0019U, /* Dance Dance Revolution Ultramix */
    0x4B4E001AU, /* Silent Scope Complete */
    0x4B4E0020U, /* TMNT - Mutant Melee */
    0x4B4E0021U, /* Dance Dance Revolution Ultramix 2 */
    0x4B4E0029U, /* Dance Dance Revolution Ultramix 3 */
    0x4B4E002FU, /* World Soccer Winning Eleven 9 */
    0x4B4E0034U, /* Wallace & Gromit - The Curse of the Were-Rabbit */
    0x4B4E0038U, /* Xiaolin Showdown */
    0x4C410004U, /* Star Wars - The Clone Wars */
    0x4C410008U, /* Gladius */
    0x4C41000BU, /* Star Wars - Jedi Knight - Jedi Academy */
    0x4C41000DU, /* Armed and Dangerous */
    0x4C410011U, /* Star Wars - Battlefront */
    0x4C410015U, /* Mercenaries */
    0x4C410017U, /* Star Wars - Episode III - Revenge of the Sith */
    0x4C41001AU, /* Star Wars - Battlefront II */
    0x4C41001FU, /* LEGO Star Wars II - The Original Trilogy */
    0x4D490006U, /* Syberia */
    0x4D49000DU, /* Sniper Elite */
    0x4D4A0003U, /* Gunmetal */
    0x4D4A0009U, /* Advent Rising */
    0x4D4A000BU, /* Drake of the 99 Dragons */
    0x4D4A000DU, /* BloodRayne 2 */
    0x4D4A0017U, /* Worms 4 - Mayhem */
    0x4D530005U, /* Amped - Freestyle Snowboarding */
    0x4D530007U, /* Azurik - Rise of Perathia */
    0x4D53000AU, /* Sudeki */
    0x4D53000FU, /* RalliSport Challenge */
    0x4D530014U, /* Tao Feng - Fist of the Lotus */
    0x4D530015U, /* Quantum Redshift */
    0x4D530035U, /* Top Spin */
    0x4D530039U, /* RalliSport Challenge 2 */
    0x4D530041U, /* Amped 2 */
    0x4D53004BU, /* Project Gotham Racing 2 */
    0x4D530053U, /* Grabbed by the Ghoulies */
    0x4D53005DU, /* Links 2004 */
    0x4D530064U, /* Halo 2 */
    0x4D53006BU, /* MechAssault 2 - Lone Wolf */
    0x4D53006EU, /* Forza Motorsport */
    0x4D530083U, /* MechAssault 2 - Lone Wolf */
    0x4D5300D4U, /* PocketBike Racer */
    0x4D570010U, /* NBA Ballers */
    0x4D570011U, /* Dr Muto */
    0x4D570012U, /* FireBlade */
    0x4D570015U, /* Suffering, The */
    0x4D570016U, /* Psi-Ops - The Mindgate Conspiracy */
    0x4D570018U, /* RoadKill */
    0x4D570019U, /* Mortal Kombat - Deception */
    0x4D570021U, /* Unreal Championship 2 - The Liandri Conflict */
    0x4D570024U, /* Narc */
    0x4D57002CU, /* Suffering, The - Ties That Bind */
    0x4D570034U, /* Mortal Kombat - Armageddon */
    0x4E4D0003U, /* Soulcalibur II */
    0x4E4D0005U, /* Dead to Rights */
    0x4E4D0006U, /* Pac-Man World 2 */
    0x4E4D000BU, /* Kill Switch */
    0x4E4D000CU, /* Spawn - Armageddon */
    0x4E4D000FU, /* I-Ninja */
    0x4E4D0015U, /* Dead to Rights II */
    0x4F580004U, /* ProStroke Golf - World Tour 2007 */
    0x52440001U, /* Yourself!Fitness */
    0x53430006U, /* Conflict - Desert Storm II - Back to Baghdad */
    0x5343000AU, /* Conflict - Vietnam */
    0x5343000CU, /* Conflict - Global Terror */
    0x5343000EU, /* Rogue Trooper */
    0x5343000FU, /* Constantine */
    0x53430010U, /* Total Overdose */
    0x534300F9U, /* Commandos - Strike Force */
    0x534300FAU, /* Hitman - Blood Money */
    0x534300FDU, /* Lara Croft Tomb Raider - Legend */
    0x534300FFU, /* Reservoir Dogs */
    0x53450003U, /* Sega GT 2002 */
    0x53450007U, /* Panzer Dragoon Orta */
    0x5345000FU, /* ToeJam & Earl III - Mission to Earth */
    0x53450013U, /* NBA 2K3 */
    0x53450014U, /* Sega Soccer Slam */
    0x5345001AU, /* World Series Baseball 2K3 */
    0x53450021U, /* Sega GT Online */
    0x53450022U, /* ESPN NFL Football */
    0x53450023U, /* ESPN NBA Basketball */
    0x53450026U, /* ESPN NHL Hockey */
    0x53450028U, /* ESPN Major League Baseball */
    0x5345002AU, /* Worms 3D */
    0x53450030U, /* ESPN NFL 2K5 */
    0x53450031U, /* ESPN NBA 2K5 */
    0x53450035U, /* Worms Forts - Under Siege */
    0x53450036U, /* OutRun 2 */
    0x53450038U, /* Super Monkey Ball Deluxe */
    0x53450088U, /* OutRun 2006 - Coast 2 Coast */
    0x53500001U, /* Combat Elite - WWII Paratroopers */
    0x54430001U, /* Dead or Alive 3 */
    0x54430003U, /* Ninja Gaiden */
    0x54430007U, /* Dead or Alive Xtreme Beach Volleyball */
    0x5443000DU, /* Ninja Gaiden Black */
    0x5443000EU, /* Tecmo Classic Arcade */
    0x544D0005U, /* World Racing */
    0x544D000AU, /* UFC - Ultimate Fighting Championship - Tapout 2 */
    0x54510003U, /* MX 2002 featuring Ricky Carmichael */
    0x54510004U, /* Dark Summit */
    0x54510007U, /* Toxic Grind */
    0x5451000DU, /* WWE Raw 2 */
    0x5451000FU, /* Alter Echo */
    0x54510010U, /* MX SuperFly */
    0x54510016U, /* MotoGP 2 */
    0x54510018U, /* Punisher, The */
    0x5451001DU, /* Broken Sword - The Sleeping Dragon */
    0x54510021U, /* MX Unleashed */
    0x54510022U, /* Full Spectrum Warrior */
    0x54510023U, /* Sphinx and the Cursed Mummy */
    0x54510024U, /* WWE WrestleMania 21 */
    0x54510025U, /* Evil Dead - Regeneration */
    0x54510026U, /* Destroy All Humans! */
    0x54510027U, /* Nickelodeon SpongeBob SquarePants - The Movie */
    0x54510028U, /* Disney-Pixar The Incredibles */
    0x54510089U, /* Hot Wheels - Stunt Track Challenge */
    0x5451008BU, /* Nickelodeon Tak 2 - The Staff of Dreams */
    0x545100EDU, /* MX vs ATV Unleashed */
    0x545100EFU, /* Juiced */
    0x545100F2U, /* Disney-Pixar Cars */
    0x545100F6U, /* Disney-Pixar The Incredibles - Rise of the Underminer */
    0x545100F8U, /* Full Spectrum Warrior - Ten Hammers */
    0x54510106U, /* Destroy All Humans! 2 */
    0x54510109U, /* Disney-Pixar Ratatouille */
    0x54540005U, /* Conflict - Desert Storm */
    0x54540006U, /* State of Emergency */
    0x54540008U, /* Midnight Club II */
    0x5454000CU, /* Max Payne 2 - The Fall of Max Payne */
    0x5454000EU, /* Grand Theft Auto III */
    0x5454000FU, /* Grand Theft Auto - Vice City */
    0x54540011U, /* Pure Pinball */
    0x54540012U, /* Close Combat - First to Fight - United States Marines */
    0x54540015U, /* Motocross Mania 3 */
    0x54540016U, /* Virtual Pool - Tournament Edition */
    0x5454001AU, /* Red Dead Revolver */
    0x5454001BU, /* Robotech - Invasion */
    0x54540073U, /* Grand Theft Auto - Vice City */
    0x54540077U, /* Ford Mustang - The Legend Lives */
    0x54540079U, /* Midnight Club 3 - DUB Edition / Midnight Club 3 - DUB Edition Remix */
    0x5454007CU, /* Spy vs Spy */
    0x5454007DU, /* Classified - The Sentinel Crisis */
    0x54540082U, /* Grand Theft Auto - San Andreas */
    0x54540083U, /* Shattered Union */
    0x54540084U, /* Spy vs Spy */
    0x54540089U, /* Ford vs Chevy */
    0x5454008AU, /* Codename - Kids Next Door - Operation - V I D E O G A M E */
    0x5454008CU, /* Zathura */
    0x54540090U, /* NBA 2K6 */
    0x5454009BU, /* Warriors, The */
    0x5454009CU, /* Hummer Badlands */
    0x545400A3U, /* Grand Theft Auto - San Andreas */
    0x545400ABU, /* Da Vinci Code, The */
    0x545400AEU, /* NBA 2K7 */
    0x55530002U, /* Rayman 3 - Hoodlum Havoc */
    0x55530004U, /* Deathrow */
    0x55530008U, /* Far Cry Instincts */
    0x55530011U, /* Myst III - Exile */
    0x55530012U, /* Dragon's Lair 3D - Return to the Lair */
    0x55530014U, /* Rocky */
    0x55530036U, /* 187 - Ride or Die */
    0x55530039U, /* Chessmaster */
    0x5553003BU, /* Prince of Persia - Warrior Within */
    0x5553003CU, /* Brothers in Arms - Road to Hill 30 */
    0x5553003FU, /* CSI - Crime Scene Investigation */
    0x55530041U, /* Tom Clancy's Splinter Cell - Chaos Theory */
    0x55530048U, /* Tom Clancy's Rainbow Six - Lockdown */
    0x5553004AU, /* Myst IV - Revelation */
    0x5553004BU, /* Cold Fear */
    0x5553004CU, /* Peter Jackson's King Kong - The Official Game of the Movie */
    0x55530054U, /* Tom Clancy's Ghost Recon - Advanced Warfighter */
    0x55530057U, /* Blazing Angels - Squadrons of WWII */
    0x55530058U, /* Prince of Persia - The Two Thrones */
    0x5553005AU, /* Brothers in Arms - Earned in Blood */
    0x5553005EU, /* Tom Clancy's Splinter Cell - Double Agent */
    0x5553005FU, /* Tom Clancy's Rainbow Six - Critical Hour */
    0x55530060U, /* Far Cry Instincts - Evolution */
    0x55530061U, /* Open Season */
    0x56550004U, /* Lord of the Rings, The - The Fellowship of the Ring */
    0x56550005U, /* Buffy the Vampire Slayer - Chaos Bleeds */
    0x5655000DU, /* Thing, The */
    0x5655000EU, /* Enclave */
    0x56550015U, /* Simpsons, The - Hit & Run */
    0x56550019U, /* Men of Valor */
    0x5655001FU, /* Crash Nitro Kart */
    0x56550021U, /* Jurassic Park - Operation Genesis */
    0x56550023U, /* Hobbit, The - The Prelude to the Lord of the Rings */
    0x56550026U, /* Chronicles of Riddick, The - Escape from Butcher Bay */
    0x56550028U, /* Dr Seuss' The Cat in the Hat */
    0x56550034U, /* Spyro - A Hero's Tail */
    0x56550036U, /* Crash Twinsanity */
    0x5655003FU, /* Scarface - The World Is Yours */
    0x56550041U, /* Crash Tag Team Racing */
    0x56550042U, /* 50 Cent - Bulletproof */
    0x56550043U, /* Eragon */
    0x5655004BU, /* Ice Age 2 - The Meltdown */
    0x56560025U, /* SWAT - Global Strike Team */
    0x57450005U, /* Curse - The Eye of Isis */
    0x57520001U, /* Justice League Heroes */
    0x584C8014U, /* Forza Motorsport */
};

#endif
