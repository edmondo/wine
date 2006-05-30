/*
 * DNS support
 *
 * Copyright (C) 2006 Hans Leidekker
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"
#include "wine/port.h"
#include "wine/debug.h"

#include <stdarg.h>
#include <string.h>
#include <sys/types.h>

#ifdef HAVE_NETINET_IN_H
# include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_NAMESER_H
# include <arpa/nameser.h>
#endif
#ifdef HAVE_RESOLV_H
# include <resolv.h>
#endif
#ifdef HAVE_NETDB_H
# include <netdb.h>
#endif

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winnls.h"
#include "windns.h"

#include "dnsapi.h"

WINE_DEFAULT_DEBUG_CHANNEL(dnsapi);

#ifdef HAVE_RESOLV

static CRITICAL_SECTION resolver_cs;
static CRITICAL_SECTION_DEBUG resolver_cs_debug =
{
    0, 0, &resolver_cs,
    { &resolver_cs_debug.ProcessLocksList,
      &resolver_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": resolver_cs") }
};
static CRITICAL_SECTION resolver_cs = { &resolver_cs_debug, -1, 0, 0, 0, 0 };

#define LOCK_RESOLVER()     do { EnterCriticalSection( &resolver_cs ); } while (0)
#define UNLOCK_RESOLVER()   do { LeaveCriticalSection( &resolver_cs ); } while (0)


static const char *dns_section_to_str( ns_sect section )
{
    switch (section)
    {
    case ns_s_qd:   return "Question";
    case ns_s_an:   return "Answer";
    case ns_s_ns:   return "Authority";
    case ns_s_ar:   return "Additional";
    default:
    {
        static char tmp[5];
        FIXME( "unknown section: 0x%02x\n", section );
        sprintf( tmp, "0x%02x", section );
        return tmp;
    }
    }
}

static unsigned long dns_map_options( DWORD options )
{
    unsigned long ret = 0;
            
    if (options & DNS_QUERY_STANDARD)
        ret |= RES_DEFAULT;
    if (options & DNS_QUERY_ACCEPT_TRUNCATED_RESPONSE)
        ret |= RES_IGNTC;
    if (options & DNS_QUERY_USE_TCP_ONLY)
        ret |= RES_USEVC;
    if (options & DNS_QUERY_NO_RECURSION)
        ret &= ~RES_RECURSE;
    if (options & DNS_QUERY_NO_LOCAL_NAME)
        ret &= ~RES_DNSRCH;
    if (options & DNS_QUERY_NO_HOSTS_FILE)
        ret |= RES_NOALIASES;
    if (options & DNS_QUERY_TREAT_AS_FQDN)
        ret &= ~RES_DEFNAMES;

    if (options & DNS_QUERY_DONT_RESET_TTL_VALUES)
        FIXME( "option DNS_QUERY_DONT_RESET_TTL_VALUES not implemented\n" );
    if (options & DNS_QUERY_RESERVED)
        FIXME( "option DNS_QUERY_RESERVED not implemented\n" );
    if (options & DNS_QUERY_WIRE_ONLY)
        FIXME( "option DNS_QUERY_WIRE_ONLY not implemented\n" );
    if (options & DNS_QUERY_NO_WIRE_QUERY)
        FIXME( "option DNS_QUERY_NO_WIRE_QUERY not implemented\n" );
    if (options & DNS_QUERY_BYPASS_CACHE)
        FIXME( "option DNS_QUERY_BYPASS_CACHE not implemented\n" );
    if (options & DNS_QUERY_RETURN_MESSAGE)
        FIXME( "option DNS_QUERY_RETURN_MESSAGE not implemented\n" );

    if (options & DNS_QUERY_NO_NETBT)
        TRACE( "netbios query disabled\n" );

    return ret;
}

static DNS_STATUS dns_map_error( int error )
{
    switch (error)
    {
    case NOERROR:  return ERROR_SUCCESS;
    case FORMERR:  return DNS_ERROR_RCODE_FORMAT_ERROR;
    case SERVFAIL: return DNS_ERROR_RCODE_SERVER_FAILURE;
    case NXDOMAIN: return DNS_ERROR_RCODE_NAME_ERROR;
    case NOTIMP:   return DNS_ERROR_RCODE_NOT_IMPLEMENTED;
    case REFUSED:  return DNS_ERROR_RCODE_REFUSED;
    case YXDOMAIN: return DNS_ERROR_RCODE_YXDOMAIN;
    case YXRRSET:  return DNS_ERROR_RCODE_YXRRSET;
    case NXRRSET:  return DNS_ERROR_RCODE_NXRRSET;
    case NOTAUTH:  return DNS_ERROR_RCODE_NOTAUTH;
    case NOTZONE:  return DNS_ERROR_RCODE_NOTZONE;
    default:
        FIXME( "unmapped error code: %d\n", error );
        return DNS_ERROR_RCODE_NOT_IMPLEMENTED;
    }
}

static DNS_STATUS dns_map_h_errno( int error )
{
    switch (error)
    {
    case NO_DATA:
    case HOST_NOT_FOUND: return DNS_ERROR_RCODE_NAME_ERROR;
    case TRY_AGAIN:      return DNS_ERROR_RCODE_SERVER_FAILURE;
    case NO_RECOVERY:    return DNS_ERROR_RCODE_REFUSED;
    case NETDB_INTERNAL: return DNS_ERROR_RCODE;
    default:
        FIXME( "unmapped error code: %d\n", error );
        return DNS_ERROR_RCODE_NOT_IMPLEMENTED;
    }
}

static char *dns_dname_from_msg( ns_msg msg, const unsigned char *pos )
{
    int len;
    char *str, dname[NS_MAXDNAME] = ".";

    /* returns *compressed* length, ignore it */
    len = dns_ns_name_uncompress( ns_msg_base( msg ), ns_msg_end( msg ),
                                  pos, dname, sizeof(dname) );

    len = strlen( dname );
    str = dns_alloc( len + 1 );
    if (str) strcpy( str, dname );
    return str;
}

static char *dns_str_from_rdata( const unsigned char *rdata )
{
    char *str;
    unsigned int len = rdata[0];

    str = dns_alloc( len + 1 );
    if (str)
    {
        memcpy( str, ++rdata, len );
        str[len] = '\0';
    }
    return str;
}

static unsigned int dns_get_record_size( ns_rr *rr )
{
    const unsigned char *pos = rr->rdata;
    unsigned int num = 0, size = sizeof(DNS_RECORDA);

    switch (rr->type)
    {
    case T_KEY:
    {
        pos += sizeof(WORD) + sizeof(BYTE) + sizeof(BYTE);
        size += rr->rdata + rr->rdlength - pos - 1;
        break;
    }
    case T_SIG:
    {
        pos += sizeof(PCHAR) + sizeof(WORD) + 2 * sizeof(BYTE);
        pos += 3 * sizeof(DWORD) + 2 * sizeof(WORD);
        size += rr->rdata + rr->rdlength - pos - 1;
        break;
    }
    case T_HINFO:
    case T_ISDN:
    case T_TXT:
    case T_X25:
    {
        while (pos[0] && pos < rr->rdata + rr->rdlength)
        {
            num++;
            pos += pos[0] + 1;
        }
        size += (num - 1) * sizeof(PCHAR);
        break;
    }
    case T_NULL:
    {
        size += rr->rdlength - 1;
        break;
    }
    case T_NXT:
    case T_WKS:
    case 0xff01:  /* WINS */
    {
        FIXME( "unhandled type: %s\n", dns_type_to_str( rr->type ) );
        break;
    }
    default:
        break;
    }
    return size;
}

static DNS_STATUS dns_copy_rdata( ns_msg msg, ns_rr *rr, DNS_RECORDA *r, WORD *dlen )
{
    DNS_STATUS ret = ERROR_SUCCESS;
    const unsigned char *pos = rr->rdata;
    unsigned int i, size;

    switch (rr->type)
    {
    case T_A:
    {
        r->Data.A.IpAddress = *(DWORD *)pos;
        *dlen = sizeof(DNS_A_DATA);
        break; 
    }
    case T_AAAA:
    {
        for (i = 0; i < sizeof(IP6_ADDRESS)/sizeof(DWORD); i++)
        {
            r->Data.AAAA.Ip6Address.IP6Dword[i] = *(DWORD *)pos;
            pos += sizeof(DWORD);
        }

        *dlen = sizeof(DNS_AAAA_DATA);
        break;
    }
    case T_KEY:
    {
        /* FIXME: byte order? */
        r->Data.KEY.wFlags      = *(WORD *)pos;   pos += sizeof(WORD);
        r->Data.KEY.chProtocol  = *(BYTE *)pos++;
        r->Data.KEY.chAlgorithm = *(BYTE *)pos++;

        size = rr->rdata + rr->rdlength - pos;

        for (i = 0; i < size; i++)
            r->Data.KEY.Key[i] = *(BYTE *)pos++;

        *dlen = sizeof(DNS_KEY_DATA) + (size - 1) * sizeof(BYTE);
        break;
    }
    case T_RP:
    case T_MINFO:
    {
        r->Data.MINFO.pNameMailbox = dns_dname_from_msg( msg, pos );
        if (!r->Data.MINFO.pNameMailbox) return ERROR_NOT_ENOUGH_MEMORY;

        if (dns_ns_name_skip( &pos, ns_msg_end( msg ) ) < 0)
            return DNS_ERROR_BAD_PACKET;

        r->Data.MINFO.pNameErrorsMailbox = dns_dname_from_msg( msg, pos );
        if (!r->Data.MINFO.pNameErrorsMailbox)
        {
            dns_free( r->Data.MINFO.pNameMailbox ); 
            return ERROR_NOT_ENOUGH_MEMORY;
        }

        *dlen = sizeof(DNS_MINFO_DATAA);
        break; 
    }
    case T_AFSDB:
    case T_RT:
    case T_MX:
    {
        r->Data.MX.wPreference = ntohs( *(WORD *)pos );
        r->Data.MX.pNameExchange = dns_dname_from_msg( msg, pos + sizeof(WORD) );
        if (!r->Data.MX.pNameExchange) return ERROR_NOT_ENOUGH_MEMORY;

        *dlen = sizeof(DNS_MX_DATAA);
        break; 
    }
    case T_NULL:
    {
        r->Data.Null.dwByteCount = rr->rdlength;
        memcpy( r->Data.Null.Data, rr->rdata, rr->rdlength );

        *dlen = sizeof(DNS_NULL_DATA) + rr->rdlength - 1;
        break;
    }
    case T_CNAME:
    case T_NS:
    case T_MB:
    case T_MD:
    case T_MF:
    case T_MG:
    case T_MR:
    case T_PTR:
    {
        r->Data.PTR.pNameHost = dns_dname_from_msg( msg, pos );
        if (!r->Data.PTR.pNameHost) return ERROR_NOT_ENOUGH_MEMORY;

        *dlen = sizeof(DNS_PTR_DATAA);
        break;
    }
    case T_SIG:
    {
        r->Data.SIG.pNameSigner = dns_dname_from_msg( msg, pos );
        if (!r->Data.SIG.pNameSigner) return ERROR_NOT_ENOUGH_MEMORY;

        if (dns_ns_name_skip( &pos, ns_msg_end( msg ) ) < 0)
            return DNS_ERROR_BAD_PACKET;

        /* FIXME: byte order? */
        r->Data.SIG.wTypeCovered  = *(WORD *)pos;   pos += sizeof(WORD);
        r->Data.SIG.chAlgorithm   = *(BYTE *)pos++;
        r->Data.SIG.chLabelCount  = *(BYTE *)pos++;
        r->Data.SIG.dwOriginalTtl = *(DWORD *)pos;  pos += sizeof(DWORD);
        r->Data.SIG.dwExpiration  = *(DWORD *)pos;  pos += sizeof(DWORD);
        r->Data.SIG.dwTimeSigned  = *(DWORD *)pos;  pos += sizeof(DWORD);
        r->Data.SIG.wKeyTag       = *(WORD *)pos;

        size = rr->rdata + rr->rdlength - pos;

        for (i = 0; i < size; i++)
            r->Data.SIG.Signature[i] = *(BYTE *)pos++;

        *dlen = sizeof(DNS_SIG_DATAA) + (size - 1) * sizeof(BYTE);
        break; 
    }
    case T_SOA:
    {
        r->Data.SOA.pNamePrimaryServer = dns_dname_from_msg( msg, pos );
        if (!r->Data.SOA.pNamePrimaryServer) return ERROR_NOT_ENOUGH_MEMORY;

        if (dns_ns_name_skip( &pos, ns_msg_end( msg ) ) < 0)
            return DNS_ERROR_BAD_PACKET;

        r->Data.SOA.pNameAdministrator = dns_dname_from_msg( msg, pos );
        if (!r->Data.SOA.pNameAdministrator)
        {
            dns_free( r->Data.SOA.pNamePrimaryServer ); 
            return ERROR_NOT_ENOUGH_MEMORY;
        }

        if (dns_ns_name_skip( &pos, ns_msg_end( msg ) ) < 0)
            return DNS_ERROR_BAD_PACKET;

        r->Data.SOA.dwSerialNo   = ntohl( *(DWORD *)pos ); pos += sizeof(DWORD);
        r->Data.SOA.dwRefresh    = ntohl( *(DWORD *)pos ); pos += sizeof(DWORD);
        r->Data.SOA.dwRetry      = ntohl( *(DWORD *)pos ); pos += sizeof(DWORD);
        r->Data.SOA.dwExpire     = ntohl( *(DWORD *)pos ); pos += sizeof(DWORD);
        r->Data.SOA.dwDefaultTtl = ntohl( *(DWORD *)pos ); pos += sizeof(DWORD);

        *dlen = sizeof(DNS_SOA_DATAA);
        break; 
    }
    case T_SRV:
    {
        r->Data.SRV.wPriority = ntohs( *(WORD *)pos ); pos += sizeof(WORD);
        r->Data.SRV.wWeight   = ntohs( *(WORD *)pos ); pos += sizeof(WORD);
        r->Data.SRV.wPort     = ntohs( *(WORD *)pos ); pos += sizeof(WORD);

        r->Data.SRV.pNameTarget = dns_dname_from_msg( msg, pos );
        if (!r->Data.SRV.pNameTarget) return ERROR_NOT_ENOUGH_MEMORY;

        *dlen = sizeof(DNS_SRV_DATAA);
        break; 
    }
    case T_HINFO:
    case T_ISDN:
    case T_X25:
    case T_TXT:
    {
        i = 0;
        while (pos[0] && pos < rr->rdata + rr->rdlength)
        {
            r->Data.TXT.pStringArray[i] = dns_str_from_rdata( pos );
            if (!r->Data.TXT.pStringArray[i])
            {
                for (--i; i >= 0; i--)
                    dns_free( r->Data.TXT.pStringArray[i] );
                return ERROR_NOT_ENOUGH_MEMORY;
            }
            i++;
            pos += pos[0] + 1;
        }
        r->Data.TXT.dwStringCount = i;
        *dlen = sizeof(DNS_TXT_DATAA) + (i - 1) * sizeof(PCHAR);
        break;
    }
    case T_ATMA:
    case T_LOC:
    case T_NXT:
    case T_TSIG:
    case T_WKS:
    case 0x00f9:  /* TKEY */
    case 0xff01:  /* WINS */
    case 0xff02:  /* WINSR */
    default:
        FIXME( "unhandled type: %s\n", dns_type_to_str( rr->type ) );
        return DNS_ERROR_RCODE_NOT_IMPLEMENTED;
    }

    return ret;
} 

static DNS_STATUS dns_copy_record( ns_msg msg, ns_sect section,
                                   unsigned short num, DNS_RECORDA **recp )
{
    DNS_STATUS ret;
    DNS_RECORDA *record;
    WORD dlen;
    ns_rr rr;

    if (dns_ns_parserr( &msg, section, num, &rr ) < 0)
        return DNS_ERROR_BAD_PACKET;

    if (!(record = dns_zero_alloc( dns_get_record_size( &rr ) )))
        return ERROR_NOT_ENOUGH_MEMORY;

    record->pName = dns_strdup_u( rr.name );
    if (!record->pName)
    {
        dns_free( record );
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    record->wType = rr.type;
    record->Flags.S.Section = section;
    record->Flags.S.CharSet = DnsCharSetUtf8;
    record->dwTtl = rr.ttl;

    if ((ret = dns_copy_rdata( msg, &rr, record, &dlen )))
    {
        dns_free( record->pName );
        dns_free( record );
        return ret;
    }
    record->wDataLength = dlen;
    *recp = record;

    TRACE( "found %s record in %s section\n",
           dns_type_to_str( rr.type ), dns_section_to_str( section ) );
    return ERROR_SUCCESS;
}

/*  The resolver lock must be held and res_init() must have been
 *  called before calling these three functions.
 */
static DNS_STATUS dns_set_serverlist( PIP4_ARRAY addrs )
{
    unsigned int i;

    if (addrs->AddrCount > MAXNS) 
    {
        WARN( "too many servers: %ld only using the first: %d\n",
              addrs->AddrCount, MAXNS );
        _res.nscount = MAXNS;
    }
    else _res.nscount = addrs->AddrCount;

    for (i = 0; i < _res.nscount; i++)
        _res.nsaddr_list[i].sin_addr.s_addr = addrs->AddrArray[i];

    return ERROR_SUCCESS;
}

static DNS_STATUS dns_get_serverlist( PIP4_ARRAY addrs, PDWORD len )
{
    unsigned int i, size;

    size = sizeof(IP4_ARRAY) + sizeof(IP4_ADDRESS) * (_res.nscount - 1);
    if (!addrs || *len < size)
    {
        *len = size;
        return ERROR_INSUFFICIENT_BUFFER;
    }

    addrs->AddrCount = _res.nscount;

    for (i = 0; i < _res.nscount; i++)
        addrs->AddrArray[i] = _res.nsaddr_list[i].sin_addr.s_addr;

    return ERROR_SUCCESS;
}

static DNS_STATUS dns_do_query( PCSTR name, WORD type, DWORD options,
                                PDNS_RECORDA *result )
{
    DNS_STATUS ret = DNS_ERROR_RCODE_NOT_IMPLEMENTED;

    unsigned int i, num;
    unsigned char answer[NS_PACKETSZ];
    ns_sect sections[] = { ns_s_an, ns_s_ar };
    ns_msg msg;

    DNS_RECORDA *record = NULL;
    DNS_RRSET rrset;
    int len;

    DNS_RRSET_INIT( rrset );

    len = __res_query( name, C_IN, type, answer, sizeof(answer) );
    if (len < 0)
    {
        ret = dns_map_h_errno( h_errno );
        goto exit;
    }

    if (dns_ns_initparse( answer, len, &msg ) < 0)
    {
        ret = DNS_ERROR_BAD_PACKET;
        goto exit;
    }

    if (ns_msg_getflag( msg, ns_f_rcode ) != ns_r_noerror)
    {
        ret = dns_map_error( ns_msg_getflag( msg, ns_f_rcode ) );
        goto exit;
    }

    for (i = 0; i < sizeof(sections)/sizeof(sections[0]); i++)
    {
        for (num = 0; num < ns_msg_count( msg, sections[i] ); num++)
        {
            ret = dns_copy_record( msg, sections[i], num, &record );
            if (ret != ERROR_SUCCESS) goto exit;

            DNS_RRSET_ADD( rrset, (DNS_RECORD *)record );
        }
    }

exit:
    DNS_RRSET_TERMINATE( rrset );

    if (ret != ERROR_SUCCESS)
        DnsRecordListFree( rrset.pFirstRR, DnsFreeRecordList );
    else
        *result = (DNS_RECORDA *)rrset.pFirstRR;

    return ret;
}

#endif /* HAVE_RESOLV */

/******************************************************************************
 * DnsQuery_A           [DNSAPI.@]
 *
 */
DNS_STATUS WINAPI DnsQuery_A( PCSTR name, WORD type, DWORD options, PIP4_ARRAY servers,
                              PDNS_RECORDA *result, PVOID *reserved )
{
    WCHAR *nameW;
    DNS_RECORDW *resultW;
    DNS_STATUS status;

    TRACE( "(%s,%s,0x%08lx,%p,%p,%p)\n", debugstr_a(name), dns_type_to_str( type ),
           options, servers, result, reserved );

    if (!name || !result)
        return ERROR_INVALID_PARAMETER;

    nameW = dns_strdup_aw( name );
    if (!nameW) return ERROR_NOT_ENOUGH_MEMORY;

    status = DnsQuery_W( nameW, type, options, servers, &resultW, reserved ); 

    if (status == ERROR_SUCCESS)
    {
        *result = (DNS_RECORDA *)DnsRecordSetCopyEx(
             (DNS_RECORD *)resultW, DnsCharSetUnicode, DnsCharSetAnsi );

        if (!*result) status = ERROR_NOT_ENOUGH_MEMORY;
        DnsRecordListFree( (DNS_RECORD *)resultW, DnsFreeRecordList );
    }

    dns_free( nameW );
    return status;
}

/******************************************************************************
 * DnsQuery_UTF8              [DNSAPI.@]
 *
 */
DNS_STATUS WINAPI DnsQuery_UTF8( PCSTR name, WORD type, DWORD options, PIP4_ARRAY servers,
                                 PDNS_RECORDA *result, PVOID *reserved )
{
    DNS_STATUS ret = DNS_ERROR_RCODE_NOT_IMPLEMENTED;
#ifdef HAVE_RESOLV

    TRACE( "(%s,%s,0x%08lx,%p,%p,%p)\n", debugstr_a(name), dns_type_to_str( type ),
           options, servers, result, reserved );

    if (!name || !result)
        return ERROR_INVALID_PARAMETER;

    LOCK_RESOLVER();

    res_init();
    _res.options |= dns_map_options( options );

    if (servers && (ret = dns_set_serverlist( servers )))
    {
        UNLOCK_RESOLVER();
        return ret;
    }

    ret = dns_do_query( name, type, options, result );

    UNLOCK_RESOLVER();

#endif
    return ret;
}

/******************************************************************************
 * DnsQuery_W              [DNSAPI.@]
 *
 */
DNS_STATUS WINAPI DnsQuery_W( PCWSTR name, WORD type, DWORD options, PIP4_ARRAY servers,
                              PDNS_RECORDW *result, PVOID *reserved )
{
    char *nameU;
    DNS_RECORDA *resultA;
    DNS_STATUS status;

    TRACE( "(%s,%s,0x%08lx,%p,%p,%p)\n", debugstr_w(name), dns_type_to_str( type ),
           options, servers, result, reserved );

    if (!name || !result)
        return ERROR_INVALID_PARAMETER;

    nameU = dns_strdup_wu( name );
    if (!nameU) return ERROR_NOT_ENOUGH_MEMORY;

    status = DnsQuery_UTF8( nameU, type, options, servers, &resultA, reserved ); 

    if (status == ERROR_SUCCESS)
    {
        *result = (DNS_RECORDW *)DnsRecordSetCopyEx(
            (DNS_RECORD *)resultA, DnsCharSetUtf8, DnsCharSetUnicode );

        if (!*result) status = ERROR_NOT_ENOUGH_MEMORY;
        DnsRecordListFree( (DNS_RECORD *)resultA, DnsFreeRecordList );
    }

    dns_free( nameU );
    return status;
}

static DNS_STATUS dns_get_hostname_a( COMPUTER_NAME_FORMAT format,
                                      LPSTR buffer, PDWORD len )
{
    char name[256];
    DWORD size = sizeof(name);

    if (!GetComputerNameExA( format, name, &size ))
        return DNS_ERROR_NAME_DOES_NOT_EXIST;

    if (!buffer || (size = lstrlenA( name ) + 1) > *len)
    {
        *len = size;
        return ERROR_INSUFFICIENT_BUFFER;
    }

    lstrcpyA( buffer, name );
    return ERROR_SUCCESS;
}

static DNS_STATUS dns_get_hostname_w( COMPUTER_NAME_FORMAT format,
                                      LPWSTR buffer, PDWORD len )
{
    WCHAR name[256];
    DWORD size = sizeof(name);

    if (!GetComputerNameExW( format, name, &size ))
        return DNS_ERROR_NAME_DOES_NOT_EXIST;

    if (!buffer || (size = lstrlenW( name ) + 1) > *len)
    {
        *len = size;
        return ERROR_INSUFFICIENT_BUFFER;
    }

    lstrcpyW( buffer, name );
    return ERROR_SUCCESS;
}

/******************************************************************************
 * DnsQueryConfig          [DNSAPI.@]
 *
 */
DNS_STATUS WINAPI DnsQueryConfig( DNS_CONFIG_TYPE config, DWORD flag, PWSTR adapter,
                                  PVOID reserved, PVOID buffer, PDWORD len )
{
    DNS_STATUS ret = ERROR_INVALID_PARAMETER;

    TRACE( "(%d,0x%08lx,%s,%p,%p,%p)\n", config, flag, debugstr_w(adapter),
           reserved, buffer, len );

    if (!len) return ERROR_INVALID_PARAMETER;

    switch (config)
    {
    case DnsConfigDnsServerList:
    {
#ifdef HAVE_RESOLV
        LOCK_RESOLVER();

        res_init();
        ret = dns_get_serverlist( (IP4_ARRAY *)buffer, len );

        UNLOCK_RESOLVER();
        break;
#else
        WARN( "compiled without resolver support\n" );
        break;
#endif
    }
    case DnsConfigHostName_A:
    case DnsConfigHostName_UTF8:
        return dns_get_hostname_a( ComputerNameDnsHostname, buffer, len );

    case DnsConfigFullHostName_A:
    case DnsConfigFullHostName_UTF8:
        return dns_get_hostname_a( ComputerNameDnsFullyQualified, buffer, len );

    case DnsConfigPrimaryDomainName_A:
    case DnsConfigPrimaryDomainName_UTF8:
        return dns_get_hostname_a( ComputerNameDnsDomain, buffer, len );

    case DnsConfigHostName_W:
        return dns_get_hostname_w( ComputerNameDnsHostname, buffer, len );

    case DnsConfigFullHostName_W:
        return dns_get_hostname_w( ComputerNameDnsFullyQualified, buffer, len );

    case DnsConfigPrimaryDomainName_W:
        return dns_get_hostname_w( ComputerNameDnsDomain, buffer, len );

    case DnsConfigAdapterDomainName_A:
    case DnsConfigAdapterDomainName_W:
    case DnsConfigAdapterDomainName_UTF8:
    case DnsConfigSearchList:
    case DnsConfigAdapterInfo:
    case DnsConfigPrimaryHostNameRegistrationEnabled:
    case DnsConfigAdapterHostNameRegistrationEnabled:
    case DnsConfigAddressRegistrationMaxCount:
        FIXME( "unimplemented config type %d\n", config );
        break;

    default:
        WARN( "unknown config type: %d\n", config );
        break;
    }
    return ret;
}
