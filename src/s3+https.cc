//-*- mode: cpp; mode: fold -*-
// Description								/*{{{*/
// $Id: http.cc,v 1.59 2004/05/08 19:42:35 mdz Exp $
/* ######################################################################

   HTTPS Acquire Method - This is the HTTPS aquire method for APT.

   It uses libcurl

   ##################################################################### */
                  /*}}}*/
// Include Files							/*{{{*/
#include <config.h>

#include <apt-pkg/fileutl.h>
#include <apt-pkg/acquire-method.h>
#include <apt-pkg/error.h>
#include <apt-pkg/hashes.h>
#include <apt-pkg/netrc.h>
#include <apt-pkg/configuration.h>

#include <sys/stat.h>
#include <sys/time.h>
#include <utime.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <iostream>
#include <sstream>

#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/bio.h>

#include "config.h"
#include "s3+https.h"
#include <apti18n.h>

#define SLEN 1024

void doEncrypt(char * kString,char * str, const char * secretKey);
                  /*}}}*/
using namespace std;

size_t 
HttpsMethod::write_data(void *buffer, size_t size, size_t nmemb, void *userp)
{
   HttpsMethod *me = (HttpsMethod *)userp;

   if(me->File->Write(buffer, size*nmemb) != true)
      return false;

   return size*nmemb;
}

int 
HttpsMethod::progress_callback(void *clientp, double dltotal, double dlnow, 
            double ultotal, double ulnow)
{
   HttpsMethod *me = (HttpsMethod *)clientp;
   if(dltotal > 0 && me->Res.Size == 0) {
      me->Res.Size = (unsigned long long)dltotal;
      me->URIStart(me->Res);
   }
   return 0;
}

void HttpsMethod::SetupProxy()  					/*{{{*/
{
   URI ServerName = Queue->Uri;

   // Determine the proxy setting - try https first, fallback to http and use env at last
   string UseProxy = _config->Find("Acquire::https::Proxy::" + ServerName.Host,
           _config->Find("Acquire::http::Proxy::" + ServerName.Host).c_str());

   if (UseProxy.empty() == true)
      UseProxy = _config->Find("Acquire::https::Proxy", _config->Find("Acquire::http::Proxy").c_str());

   // User want to use NO proxy, so nothing to setup
   if (UseProxy == "DIRECT")
      return;

   if (UseProxy.empty() == false) 
   {
      // Parse no_proxy, a comma (,) separated list of domains we don't want to use
      // a proxy for so we stop right here if it is in the list
      if (getenv("no_proxy") != 0 && CheckDomainList(ServerName.Host,getenv("no_proxy")) == true)
   return;
   } else {
      const char* result = getenv("http_proxy");
      UseProxy = result == NULL ? "" : result;
   }

   // Determine what host and port to use based on the proxy settings
   if (UseProxy.empty() == false) 
   {
      Proxy = UseProxy;
      if (Proxy.Port != 1)
   curl_easy_setopt(curl, CURLOPT_PROXYPORT, Proxy.Port);
      curl_easy_setopt(curl, CURLOPT_PROXY, Proxy.Host.c_str());
   }
}									/*}}}*/

void doEncrypt(char *kString, char *sigString, const char* secretKey){
   HMAC_CTX hctx;
   BIO *bio, *b64;
   char *sigptr;
   long siglen = -1;
   unsigned int rizlen;
   char skey[SLEN];
   unsigned char results[SLEN];

   // Initialize SHA1 encryption
   sprintf(skey, "%s", secretKey);
   HMAC_CTX_init(&hctx);
   HMAC_Init(&hctx, skey, (int)strlen((char *)skey), EVP_sha1());

   // Encrypt
   HMAC(EVP_sha1(), skey, (int)strlen((char *)skey), (unsigned char *)kString,
         (int)strlen((char *)kString), results, &rizlen);

   // Base 64 Encode
   b64 = BIO_new(BIO_f_base64());
   bio = BIO_new(BIO_s_mem());
   BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
   bio = BIO_push(b64, bio);
   BIO_write(bio, results, rizlen);
   BIO_flush(bio);

   siglen = BIO_get_mem_data(bio, &sigptr);
   memcpy(sigString, sigptr, siglen);
   sigString[siglen] = '\0';

   // Clean up Encryption, Encoding
   BIO_free_all(bio);
   HMAC_CTX_cleanup(&hctx);
}

// HttpsMethod::Fetch - Fetch an item					/*{{{*/
// ---------------------------------------------------------------------
/* This adds an item to the pipeline. We keep the pipeline at a fixed
   depth. */
bool HttpsMethod::Fetch(FetchItem *Itm)
{
   struct stat SBuf;
   struct curl_slist *headers=NULL;  
   char curl_errorstr[CURL_ERROR_SIZE];
   long curl_responsecode;
   URI Uri = Itm->Uri;
   string remotehost = Uri.Host;
   string parsed_uri = Uri;
   
   // S3 Plus hack and s3 URI mod
   
   parsed_uri.erase(0, 3);
   
   string normalized_uri = QuoteString(parsed_uri, "~");
   size_t f_plus = normalized_uri.find("+");
   size_t f_rep;
   while (f_plus != string::npos){
      f_rep = f_plus;
      normalized_uri.replace(f_rep, 1, "%2b");
      f_plus = normalized_uri.find("+", f_plus + 1);
   }

   // Same with path
   string normalized_path = QuoteString(Uri.Path, "~");
   size_t fp_plus = normalized_path.find("+");
   size_t fp_rep;
   while (fp_plus != string::npos){
      fp_rep = fp_plus;
      normalized_path.replace(fp_rep, 1, "%2b");
      fp_plus = normalized_path.find("+", f_plus + 1);
   }

   // TODO:
   //       - http::Pipeline-Depth
   //       - error checking/reporting
   //       - more debug options? (CURLOPT_DEBUGFUNCTION?)

   curl_easy_reset(curl);
   SetupProxy();

   maybe_add_auth (Uri, _config->FindFile("Dir::Etc::netrc"));

   // callbacks
   curl_easy_setopt(curl, CURLOPT_URL, static_cast<string>(normalized_uri).c_str());
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
   curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, progress_callback);
   curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, this);
   curl_easy_setopt(curl, CURLOPT_NOPROGRESS, false);
   curl_easy_setopt(curl, CURLOPT_FAILONERROR, true);
   curl_easy_setopt(curl, CURLOPT_FILETIME, true);
   // curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

   // SSL parameters are set by default to the common (non mirror-specific) value
   // if available (or a default one) and gets overload by mirror-specific ones.

   // File containing the list of trusted CA.
   string cainfo = _config->Find("Acquire::https::CaInfo","");
   string knob = "Acquire::https::"+remotehost+"::CaInfo";
   cainfo = _config->Find(knob.c_str(),cainfo.c_str());
   if(cainfo.empty() == false)
      curl_easy_setopt(curl, CURLOPT_CAINFO,cainfo.c_str());

   // Check server certificate against previous CA list ...
   bool peer_verify = _config->FindB("Acquire::https::Verify-Peer",true);
   knob = "Acquire::https::" + remotehost + "::Verify-Peer";
   peer_verify = _config->FindB(knob.c_str(), peer_verify);
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, peer_verify);

   // ... and hostname against cert CN or subjectAltName
   bool verify = _config->FindB("Acquire::https::Verify-Host",true);
   knob = "Acquire::https::"+remotehost+"::Verify-Host";
   verify = _config->FindB(knob.c_str(),verify);
   int const default_verify = (verify == true) ? 2 : 0;
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, default_verify);

   // Also enforce issuer of server certificate using its cert
   string issuercert = _config->Find("Acquire::https::IssuerCert","");
   knob = "Acquire::https::"+remotehost+"::IssuerCert";
   issuercert = _config->Find(knob.c_str(),issuercert.c_str());
   if(issuercert.empty() == false)
      curl_easy_setopt(curl, CURLOPT_ISSUERCERT,issuercert.c_str());

   // For client authentication, certificate file ...
   string pem = _config->Find("Acquire::https::SslCert","");
   knob = "Acquire::https::"+remotehost+"::SslCert";
   pem = _config->Find(knob.c_str(),pem.c_str());
   if(pem.empty() == false)
      curl_easy_setopt(curl, CURLOPT_SSLCERT, pem.c_str());

   // ... and associated key.
   string key = _config->Find("Acquire::https::SslKey","");
   knob = "Acquire::https::"+remotehost+"::SslKey";
   key = _config->Find(knob.c_str(),key.c_str());
   if(key.empty() == false)
      curl_easy_setopt(curl, CURLOPT_SSLKEY, key.c_str());

   // Allow forcing SSL version to SSLv3 or TLSv1 (SSLv2 is not
   // supported by GnuTLS).
   long final_version = CURL_SSLVERSION_DEFAULT;
   string sslversion = _config->Find("Acquire::https::SslForceVersion","");
   knob = "Acquire::https::"+remotehost+"::SslForceVersion";
   sslversion = _config->Find(knob.c_str(),sslversion.c_str());
   if(sslversion == "TLSv1")
     final_version = CURL_SSLVERSION_TLSv1;
   else if(sslversion == "SSLv3")
     final_version = CURL_SSLVERSION_SSLv3;
   curl_easy_setopt(curl, CURLOPT_SSLVERSION, final_version);

   // CRL file
   string crlfile = _config->Find("Acquire::https::CrlFile","");
   knob = "Acquire::https::"+remotehost+"::CrlFile";
   crlfile = _config->Find(knob.c_str(),crlfile.c_str());
   if(crlfile.empty() == false)
      curl_easy_setopt(curl, CURLOPT_CRLFILE, crlfile.c_str());

   // cache-control
   if(_config->FindB("Acquire::https::No-Cache",
  _config->FindB("Acquire::http::No-Cache",false)) == false)
   {
      // cache enabled
      if (_config->FindB("Acquire::https::No-Store",
    _config->FindB("Acquire::http::No-Store",false)) == true)
   headers = curl_slist_append(headers,"Cache-Control: no-store");
      stringstream ss;
      ioprintf(ss, "Cache-Control: max-age=%u", _config->FindI("Acquire::https::Max-Age",
    _config->FindI("Acquire::http::Max-Age",0)));
      headers = curl_slist_append(headers, ss.str().c_str());
   } else {
      // cache disabled by user
      headers = curl_slist_append(headers, "Cache-Control: no-cache");
      headers = curl_slist_append(headers, "Pragma: no-cache");
   }
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

   // speed limit
   int const dlLimit = _config->FindI("Acquire::https::Dl-Limit",
    _config->FindI("Acquire::http::Dl-Limit",0))*1024;
   if (dlLimit > 0)
      curl_easy_setopt(curl, CURLOPT_MAX_RECV_SPEED_LARGE, dlLimit);

   // set header
   curl_easy_setopt(curl, CURLOPT_USERAGENT,
  _config->Find("Acquire::https::User-Agent",
    _config->Find("Acquire::http::User-Agent",
      "Debian S3-CURL/1.0 (" VERSION ")").c_str()).c_str());

   // set timeout
   int const timeout = _config->FindI("Acquire::https::Timeout",
    _config->FindI("Acquire::http::Timeout",120));
   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout);
   //set really low lowspeed timeout (see #497983)
   curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, DL_MIN_SPEED);
   curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, timeout);

   // set redirect options and default to 10 redirects
   bool const AllowRedirect = _config->FindB("Acquire::https::AllowRedirect",
  _config->FindB("Acquire::http::AllowRedirect",true));
   curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, AllowRedirect);
   curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10);

   // debug
   if(_config->FindB("Debug::Acquire::https", false))
      curl_easy_setopt(curl, CURLOPT_VERBOSE, true);

   // error handling
   curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_errorstr);

   // If we ask for uncompressed files servers might respond with content-
   // negotation which lets us end up with compressed files we do not support,
   // see 657029, 657560 and co, so if we have no extension on the request
   // ask for text only. As a sidenote: If there is nothing to negotate servers
   // seem to be nice and ignore it.
   if (_config->FindB("Acquire::https::SendAccept", _config->FindB("Acquire::http::SendAccept", true)) == true)
   {
      size_t const filepos = Itm->Uri.find_last_of('/');
      string const file = Itm->Uri.substr(filepos + 1);
      if (flExtension(file) == file)
   headers = curl_slist_append(headers, "Accept: text/*");
   }

   // if we have the file send an if-range query with a range header
   if (stat(Itm->DestFile.c_str(),&SBuf) >= 0 && SBuf.st_size > 0)
   {
      char Buf[1000];
      sprintf(Buf, "Range: bytes=%li-", (long) SBuf.st_size - 1);
      headers = curl_slist_append(headers, Buf);
      sprintf(Buf, "If-Range: %s", TimeRFC1123(SBuf.st_mtime).c_str());
      headers = curl_slist_append(headers, Buf);
   }
   else if(Itm->LastModified > 0)
   {
      curl_easy_setopt(curl, CURLOPT_TIMECONDITION, CURL_TIMECOND_IFMODSINCE);
      curl_easy_setopt(curl, CURLOPT_TIMEVALUE, Itm->LastModified);
   }
   
   // S3 fun
   time_t rawtime = 0;
   struct tm * timeinfo = NULL;
   char buffer [80] = { 0 };
   char* wday = NULL;

   time( &rawtime);
   timeinfo = gmtime( &rawtime);

   // strftime does not seem to honour set_locale(LC_ALL, "") or
   // set_locale(LC_TIME, ""). So convert day of week by hand.
   switch (timeinfo->tm_wday) {
   case 0: wday = (char*)"Sun"; break;
   case 1: wday = (char*)"Mon"; break;
   case 2: wday = (char*)"Tue"; break;
   case 3: wday = (char*)"Wed"; break;
   case 4: wday = (char*)"Thu"; break;
   case 5: wday = (char*)"Fri"; break;
   case 6: wday = (char*)"Sat"; break;
   }

   strcat(buffer, wday);
   strftime(buffer+3, 80, ", %d %b %Y %T %z", timeinfo);
   string dateString((const char*)buffer);
   
   headers = curl_slist_append(headers, ("Date: " + dateString).c_str());

   string extractedPassword;
   if(getenv("AWS_SECRET_ACCESS_KEY") != NULL) {
     extractedPassword = getenv("AWS_SECRET_ACCESS_KEY");
   } else if(!Uri.Password.empty()) {
     if(Uri.Password.at(0) == '['){
       extractedPassword = Uri.Password.substr(1,Uri.Password.size()-2);
     }else{
       extractedPassword = Uri.Password;
     }
   }

   char headertext[SLEN], signature[SLEN];
   sprintf(headertext,"GET\n\n\n%s\n%s", dateString.c_str(), normalized_path.c_str());
   doEncrypt(headertext, signature, extractedPassword.c_str());

   string signatureString(signature);
   string user;
   if (getenv("AWS_ACCESS_KEY_ID") != NULL) {
     user = getenv("AWS_ACCESS_KEY_ID");
   } else if(!Uri.User.empty()) {
     user = Uri.User;
   }

   if(!user.empty() || !extractedPassword.empty())
     headers = curl_slist_append(headers, ("Authorization: AWS " + user + ":" + signatureString).c_str());
   // end S3 fun

   // go for it - if the file exists, append on it
   File = new FileFd(Itm->DestFile, FileFd::WriteAny);
   if (File->Size() > 0)
      File->Seek(File->Size() - 1);

   // keep apt updated
   Res.Filename = Itm->DestFile;

   // get it!
   CURLcode success = curl_easy_perform(curl);
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &curl_responsecode);

   long curl_servdate;
   curl_easy_getinfo(curl, CURLINFO_FILETIME, &curl_servdate);

   File->Close();

   // cleanup
   if(success != 0) 
   {
      _error->Error("%s", curl_errorstr);
      // unlink, no need keep 401/404 page content in partial/
      unlink(File->Name().c_str());
      Fail();
      return true;
   }

   // Timestamp
   struct utimbuf UBuf;
   if (curl_servdate != -1) {
       UBuf.actime = curl_servdate;
       UBuf.modtime = curl_servdate;
       utime(File->Name().c_str(),&UBuf);
   }

   // check the downloaded result
   struct stat Buf;
   if (stat(File->Name().c_str(),&Buf) == 0)
   {
      Res.Filename = File->Name();
      Res.LastModified = Buf.st_mtime;
      Res.IMSHit = false;
      if (curl_responsecode == 304)
      {
   unlink(File->Name().c_str());
   Res.IMSHit = true;
   Res.LastModified = Itm->LastModified;
   Res.Size = 0;
   URIDone(Res);
   return true;
      }
      Res.Size = Buf.st_size;
   }

   // take hashes
   Hashes Hash;
   FileFd Fd(Res.Filename, FileFd::ReadOnly);
   Hash.AddFD(Fd.Fd(), Fd.Size());
   Res.TakeHashes(Hash);

   // keep apt updated
   URIDone(Res);

   // cleanup
   Res.Size = 0;
   delete File;
   curl_slist_free_all(headers);

   return true;
};

int main()
{
   setlocale(LC_ALL, "");

   HttpsMethod Mth;
   curl_global_init(CURL_GLOBAL_SSL) ;

   return Mth.Run();
}
