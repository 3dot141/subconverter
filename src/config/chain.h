#ifndef CHAIN_H_INCLUDED
#define CHAIN_H_INCLUDED

#include "def.h"

struct ChainConfig
{
    String Name;       // chain name == derived group name == ruleset policy name
    String Front;      // "[]GroupName" reference, or node-remark regex
    String Landing;    // node tag or regex; must resolve to exactly one node
    String FrontType;  // optional helper-group type when Front is a regex; default "select"
};
using ChainConfigs = std::vector<ChainConfig>;

#endif // CHAIN_H_INCLUDED
