
#ifndef _SERIALIZE_H_INCLUDED_
#define _SERIALIZE_H_INCLUDED_

void luaser_encode(lua_State *L, int idx);
void luaser_decode(lua_State *L, const char *buf, size_t len);

#endif /* _SERIALIZE_H_INCLUDED_ */
