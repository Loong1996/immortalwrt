#!/usr/bin/env python3
"""Render page.html as a standalone preview with a fake device behind it.

Usage: preview.py page.html [preview.html]

Opens in any browser straight from disk: every request the page makes
(/info, /check, /log, /env, /ping, /reboot, the POST) is answered by a stub,
and a small bar at the bottom right switches the device between the
states the page has to handle -- healthy, no UBI, no fip volume, a build
without console recording, /info failing -- picks how a submit ends, and
can cut the connection so the heartbeat's disconnected overlay can be
seen.  The stub also goes quiet for as long as a real device would: a
write, a reboot, a whole-chip read.  Downloads have no device behind
them off a file:// page, so the transfer is replaced with the line the
page shows while the device reads.  Look at the
page here before touching the C side; what the stub answers is what the
real endpoints answer, so a layout or wording change is decided on the
same data.

Markers are handled the way gen.py does, except that the @@MACROS@@ get
sample values.  The STOCK section is shown by default; --no-stock drops it
instead, which is what a board built without CMD_HTTPD_STOCK_RESTORE serves.
Anything reachable from outside that section has to keep working in both.
Likewise --no-pnand drops the PNAND section, as an SPI NAND board serves
the page.

The page goes through jsmin.py as gen.py does -- scripts minified, CSS
comments dropped -- so the jsdom cases run what the board sends; --raw
leaves it as written, for reading.
"""
import json
import sys

from jsmin import minify

MACROS = {
    "WEB_VERSION": "1.0.1",
    "AUTHOR": "Loong",
    "AUTHOR_HOST": "github.com/Loong1996",
    "AUTHOR_URL": "https://github.com/Loong1996",
    "PROJECT_URL": "https://github.com/Loong1996/ImmortalWrt-Airoha",
    "PROJECT_HOST": "github.com/Loong1996/ImmortalWrt-Airoha",
    "PORTAL_URL": "https://loong1996.github.io/ImmortalWrt-Airoha/",
    "PORTAL_HOST": "loong1996.github.io/ImmortalWrt-Airoha",
}

# s = reserved_pebs * leb_size；u = used_bytes，dynamic 卷恒等于 s，
# 只有 static 卷（这里是 fip）的 u 才是真实内容长度。
VOLS = [
    {"i": 0, "n": "fip", "t": "static", "s": 1142784, "u": 325632},
    {"i": 1, "n": "fit", "t": "dynamic", "s": 13078528, "u": 13078528},
    {"i": 2, "n": "ubootenv", "t": "dynamic", "s": 126976, "u": 126976},
    {"i": 3, "n": "ubootenv2", "t": "dynamic", "s": 126976, "u": 126976},
    {"i": 4, "n": "bosa", "t": "dynamic", "s": 380928, "u": 380928},
    {"i": 5, "n": "ri", "t": "dynamic", "s": 380928, "u": 380928},
    {"i": 6, "n": "rootfs_data", "t": "dynamic", "s": 239349760,
     "u": 239349760},
]

INFO = {
    "web": MACROS["WEB_VERSION"],
    "model": "Nokia XG-040G-MD",
    "soc": "airoha,an7581",
    "ram": 536870912,
    "mac": "90:03:2e:12:34:56",
    "net": {"ip": "192.168.1.1", "mask": "0.0.0.0",
            "gw": "0.0.0.0", "server": "192.168.1.254",
            "dev": "airoha-gdm1", "offer": 1, "ack": 1,
            "mode": "server", "ram": 0, "saved": None, "dgw": 1,
            "client": "a4:5e:60:11:22:33"},
    "ports": [{"p": 1, "link": 0, "speed": 0, "fd": 0},
              {"p": 2, "link": 1, "speed": 1000, "fd": 1},
              {"p": 3, "link": 0, "speed": 0, "fd": 0},
              {"p": 4, "link": 1, "speed": 100, "fd": 1}],
    "uboot": "U-Boot 2026.07-ImmortalWrt (Sep 06 2026 - 10:21:03 +0800)",
    "flash": {"name": "spi-nand0", "size": 268435456, "erase": 131072,
              "page": 2048},
    "parts": [{"n": "bl2", "o": 0, "s": 131072},
              {"n": "ubi", "o": 131072, "s": 268304384}],
    "uploadmax": 0xf8d1000,
    "stock": 1,
    "log": 1,
    "fv": [{"n": "ri", "s": 262144}, {"n": "bosa", "s": 262144}],
    "ubi": {"leb": 126976, "pebs": 2046, "avail": 0, "fip": 1,
            "vols": VOLS},
}

CHECK = [
    ["闪存", 0, "spi-nand0，256 MiB，擦除块 128 KiB，页 2048 B", "闪存"],
    ["坏块", 0, "无", "闪存"],
    ["BL2", 0, "0x800 处有 BL2 镜像", "引导"],
    ["web_uboot_envver", 0, "10，与当前 U-Boot 一致", "引导"],
    ["bootcmd", 0, "与当前版本默认值一致", "引导"],
    ["引导菜单", 0, "9 项", "引导"],
    ["UBI", 0, "7 个卷，坏块 0 个，空闲 0 个逻辑擦除块", "UBI"],
    ["可写空间", 0, "刷机可用 240 MiB（1988 个逻辑擦除块）。当前空闲 0 MiB；写入固件时会先删掉 fit 与 rootfs_data，再腾出 240 MiB", "UBI"],
    ["磨损", 0, "擦写次数最大 47、平均 12", "UBI"],
    ["fip 卷", 0, "325632 字节，校验通过", "UBI"],
    ["fit 卷", 0, "FIT 镜像，12984320 字节", "UBI"],
    ["固件", 0, "ARM64 ImmortalWrt nokia_xg-040g-md FIT (Flattened Image Tree)"
               "，2026-09-05 17:01", "UBI"],
    ["ubootenv 卷", 0, "存在，CRC 0x3f2a91c4", "环境"],
    ["ubootenv2 卷", 0, "存在，与 ubootenv 一致", "环境"],
    ["ri 卷", 0, "已读取，MAC 90:03:2e:12:34:56", "出厂数据"],
    ["bosa 卷", 1, "已读取，内容为空", "出厂数据"],
    ["U-Boot MAC", 0, "90:03:2e:12:34:56，与出厂数据一致", "出厂数据"],
    ["流水灯", 0, "5 个：green:power、green:wan、green:wan-online、green:usb-1、green:usb-2", "指示灯"],
]
# FiberHome HG5382A: parallel NAND, so the SoC does the ECC and the whole-chip
# backup reads raw with OOB.  "pnand" is the first boot from stock (the stock
# UBI does not read back here, the stock format was just probed); "pnandmig" is
# the same board after the migration, format saved in the environment.
PNAND_INFO = {
    "model": "FiberHome HG5382A",
    "flash": {"name": "nand0", "size": 268435456, "erase": 131072,
              "page": 2048, "oob": 128, "cecc": 1},
    "parts": [{"n": "bl2", "o": 0, "s": 131072},
              {"n": "ubi", "o": 131072, "s": 268304384}],
    "fv": [{"n": "factory", "s": 0x100000}],
    "sfmt": {"st": "ok", "ecc": 8, "spare": 28, "inv": 0, "fdm": 8,
             "fecc": 1, "swap": None, "n": 48, "k": 0, "src": "probe",
             "saved": 0},
    "ubi": {"leb": 126976, "pebs": 2046, "avail": 0, "fip": 1,
            "vols": [
                {"i": 0, "n": "fip", "t": "static", "s": 1142784,
                 "u": 417792},
                {"i": 1, "n": "ubootenv", "t": "dynamic", "s": 126976,
                 "u": 126976},
                {"i": 2, "n": "ubootenv2", "t": "dynamic", "s": 126976,
                 "u": 126976},
                {"i": 3, "n": "factory", "t": "dynamic", "s": 1142784,
                 "u": 1142784},
                {"i": 4, "n": "fit", "t": "dynamic", "s": 13078528,
                 "u": 13078528},
                {"i": 5, "n": "rootfs_data", "t": "dynamic",
                 "s": 237699072, "u": 237699072}]},
}

ENV = [
    ("arch", "arm"),
    ("baudrate", "115200"),
    ("board", "an7581"),
    ("boot_ubi", "ubi part ubi && ubi read $loadaddr fit && bootm $loadaddr"),
    ("bootcmd", "run _firstboot ; run boot_ubi ; run web_uboot_boot_forever"),
    ("bootdelay", "3"),
    ("bootmenu_0", "启动 ImmortalWrt.=run boot_ubi"),
    ("bootmenu_8", "网页恢复（Airoha Web U-Boot 1.0.1）.=httpd"),
    ("bootmenu_delay", "3"),
    ("check_buttons", "if button reset ; then echo recovery ; httpd ; fi"),
    ("ethaddr", "90:03:2e:12:34:56"),
    ("ethaddr_factory", "90:03:2e:12:34:56"),
    ("fdtcontroladdr", "bfad0f10"),
    ("ipaddr", "192.168.1.1"),
    ("loadaddr", "0x84000000"),
    ("netmask", "255.255.255.0"),
    ("serverip", "192.168.1.100"),
    ("soc", "airoha"),
    ("stderr", "serial"),
    ("stdin", "serial"),
    ("stdout", "serial"),
    ("ubi_write_fip", "run ubi_remove_rootfs ; ubi check fip && ubi remove "
                      "fip ; ubi create fip 0x100000 static && ubi write "
                      "$loadaddr fip $filesize"),
    ("ubi_write_production", "ubi check fit && ubi remove fit ; "
                             "ubi check rootfs_data && ubi remove rootfs_data ; "
                             "ubi create fit $filesize dynamic && "
                             "ubi write $loadaddr fit $filesize"),
    ("vendor", "nokia"),
    ("web_uboot_boot_forever", "while true ; do httpd ; sleep 1 ; done"),
    ("web_uboot_envver", "10"),
    ("web_uboot_format_ubi", "ubi detach ; mtd erase ubi && ubi part ubi"),
    ("web_uboot_write_bl2", "mtd erase bl2 && mtd write bl2 $loadaddr 0x800 $filesize"),
    ("web_uboot_write_fip", "if ubi check fip ; then ubi write $loadaddr fip "
                            "$filesize ; else run ubi_write_fip ; fi"),
]

LOG = """

U-Boot 2026.07-ImmortalWrt-r40957-4b007b8c20 (Sep 05 2026 - 17:01:01 +0000)

CPU:   Airoha AN7581
dram: probing by address aliasing, base 0x80000000
dram:  anchor at 0x80200000, holds 0x00000000
dram:   512 MiB: wrote 0xa0200000, anchor now 0xa5a55a5a -- wrapped onto the anchor
dram: 512 MiB, agrees with the device tree
DRAM:  512 MiB
Core:  37 devices, 22 uclasses, devicetree: separate
Loading Environment from UBI... spi-nand: spi_nand nand@0: SkyHigh SPI NAND was found.
spi-nand: spi_nand nand@0: 256 MiB, block size: 128 KiB, page size: 2048, OOB size: 128
Read 126976 bytes from volume ubootenv to 00000000bfad17c0
OK
In:    serial
Out:   serial
Err:   serial
Net:   eth0: airoha-gdm1
httpd: bouncing link on 1 port(s) so the PC asks for an address again
Airoha Web U-Boot %s by Loong
Using airoha-gdm1 device, MAC 90:03:2e:12:34:56
Listening for HTTP on 192.168.1.1 port 80
Handing out DHCP leases from 192.168.1.1
Press Ctrl-C to abort
httpd: DHCP OFFER 192.168.1.100 -> 3c:7c:3f:1a:2b:3c
httpd: DHCP ACK 192.168.1.100 -> 3c:7c:3f:1a:2b:3c
""" % MACROS["WEB_VERSION"]

# The stub.  Plain ES5 like the page itself.
STUB = r"""
<script>(function(){
var D=@DATA@,S={dev:'ok',post:'ok',conn:'up',sf:'ok',dd:'ok'},T0=Date.now(),DOWN=0;

/*
 * 擦尾报的块数。设备是从镜像占到的最后一个擦除块之后起算的，这里照抄，
 * 否则完成页上的数字对不上，那一行就白加了。
 */
function wiped(u,tot){
 if(!/wipe=1/.test(u||''))return '';
 var f=D.info.flash,off=parseInt((/off=0x([0-9a-f]+)/.exec(u||'')||[0,'0'])[1],16),
     first=off+Math.ceil(tot/f.erase)*f.erase,
     nb=Math.max(0,Math.floor((f.size-first)/f.erase));
 return ' wiped '+nb}
/* 识别、核对、保存之后设备那份 sfmt 就换了 */
var SFCUR=null;
/* 样本页：全片均匀挑 48 页；「两块被挪过」的就是其中这两页所在的块 */
var SFPG=[],SFMOVED=[0x2a00000,0x9c00000];
(function(){for(var q=0;q<48;q++)SFPG.push(0x100000+q*0x520000);SFPG[3]=SFMOVED[0];SFPG[20]=SFMOVED[1]})();
/* 桩里「原厂真正的格式」：试读和核对拿它比 */
var SFTRUE='8,28,0,8,1';
var DSEQ=0,DINFO={seq:0,len:0,crc:'00000000',holes:0,name:''};
var DBUSY=0,DSENT=0,DTOTAL=0,DTICK=null;
/* 真设备的日志会一直长，跟随功能不自己长就看不出在跟 */
var LOGX='',LOGSEQ=0;
setInterval(function(){LOGSEQ++;
LOGX+='httpd: DHCP ACK 192.168.1.100 -> 3c:7c:3f:1a:2b:3'+(LOGSEQ%9)+'\n'},3000);
function down(){return S.conn=='down'||Date.now()<DOWN}
function fall(ms){DOWN=Date.now()+ms}
function info(){var i=JSON.parse(JSON.stringify(D.info)),k;
if(S.dev.indexOf('pnand')==0){for(k in D.pnand)i[k]=JSON.parse(JSON.stringify(D.pnand[k]));
 /* 原厂那次：原厂 UBI 读不出；已迁移那次：格式早已存进环境变量，dd 也核对过 */
 if(S.dev=='pnand')i.ubi=null;else{i.sfmt.src='dd';i.sfmt.saved=1;i.sfmt.swap=0}
 if(S.sf=='amb')i.sfmt={st:'amb',k:2};else if(S.sf!='ok')i.sfmt={st:S.sf};
 /* 已迁移的板子闪存早不是原厂内容了，但存下来的那份还在 */
 if(S.dev=='pnandmig'&&S.sf=='na'){i.sfmt=JSON.parse(JSON.stringify(D.pnand.sfmt));i.sfmt.st='ok';i.sfmt.src='dd';i.sfmt.saved=1;i.sfmt.swap=0}
 if(SFCUR)i.sfmt=JSON.parse(JSON.stringify(SFCUR))}
if(S.dev=='noubi')i.ubi=null;
/*
 * U-Boot 不肯挂（补丁 205）：ecc 是一部分已是本格式、一部分还是 ECC8 的混合状态，
 * 扫到 5% 就停；eccall 是整片别的格式，抽样就拒；foreign 是原厂布局
 */
if(S.dev=='ubiecc'){i.ubi=null;i.ubirefuse={why:'ecc',n:102}}
if(S.dev=='ubieccall'){i.ubi=null;i.ubirefuse={why:'eccall',n:17}}
if(S.dev=='ubiforeign'){i.ubi=null;i.ubirefuse={why:'foreign',n:1151}}
if(S.dev=='nofip'){i.ubi.fip=0;i.ubi.vols=i.ubi.vols.filter(function(v){return v.n!='fip'})}
if(S.dev=='nolog')i.log=0;
return i}
function check(){var c=D.check.map(function(r){return{n:r[0],s:r[1],v:r[2],g:r[3]}});
if(S.dev=='noubi'||S.dev=='pnand'||S.dev.indexOf('ubi')==0)return c.filter(function(i){return i.g=='闪存'||i.g=='引导'}).concat([
{n:'UBI',s:2,v:S.dev=='ubiecc'?'没有挂载：至少 102 块是按别的 ECC 格式写的，为保护数据一块都没擦。可先备份，再重建 UBI 或强制挂载，见页面顶部'
:S.dev=='ubieccall'?'没有挂载：闪存上的数据不是按本 U-Boot 的 ECC 格式写的，为保护数据一块都没擦。可先备份，再重建 UBI，见页面顶部'
:S.dev=='ubiforeign'?'没有挂载：至少 1151 块是 UBI 没写过的数据，又没有 fip 卷，为保护数据一块都没擦。可先备份，再重建 UBI 或强制挂载，见页面顶部'
:'无法挂载，闪存上无可用的 UBI。首次迁移请在「引导升级」页启用「重建 UBI」，并同时上传 BL2、U-Boot 与固件',g:'UBI'},
{n:'U-Boot MAC',s:0,v:'90:03:2e:12:34:56',g:'出厂数据'},
{n:'流水灯',s:0,v:'5 个：green:power、green:wan、green:wan-online、green:usb-1、green:usb-2',g:'指示灯'}]);
if(S.dev=='nofip')c.forEach(function(i){if(i.n=='fip 卷'){i.s=2;i.v='不存在。当前 U-Boot 仅存于内存，请在「引导升级」页上传 U-Boot 文件'}});
return c}
function ip4(s){var a=/^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(s||''),i;
 if(!a)return 0;
 for(i=1;i<5;i++)if(+a[i]>255)return 0;
 return 1}
/* 掩码得是连成一片的前导 1：取反加一还等于取反本身的补，10.0.0.1 这种就过不去 */
function maskok(s){var p=s.split('.'),n=0,m,i;
 for(i=0;i<4;i++)n=n*256+(+p[i]);
 m=~n>>>0;return ((m+1)&m)===0}
function body(u){
 if(u=='/ping')return JSON.stringify({up:Date.now()-T0,ovf:0});
 if(u=='/dumpinfo')return JSON.stringify(
  {seq:DINFO.seq,len:DINFO.len,crc:DINFO.crc,holes:DINFO.holes,
   name:DINFO.name,busy:DBUSY,sent:DSENT,total:DBUSY?DTOTAL:0});
 if(u=='/info')return S.dev=='noinfo'?null:JSON.stringify(info());
 /* 网络这一页轮询的那一半：地址与端口，不含 UBI */
 if(u=='/net')return JSON.stringify({net:D.info.net,ports:D.info.ports});
 /* 三种模式走同一个端点；server 那一档掩码与末位由设备定死 */
 if(u.indexOf('/netmode')==0){
  var mo=/[?&]mode=([^&]*)/.exec(u),g=/[?&]ip=([^&]*)/.exec(u),
      k=/[?&]mask=([^&]*)/.exec(u),sv=/[?&]save=1(&|$)/.test(u),
      md=mo?mo[1]:'',ip=g?decodeURIComponent(g[1]):'',
      mk=k?decodeURIComponent(k[1]):'';
  if(md!='server'&&md!='static'&&md!='client')return 'bad mode';
  if(md=='client'){D.info.net.mode='client';D.info.net.ram=sv?0:1;
   if(sv)D.info.net.saved={mode:'client'};
   return 'ok client - - '+(sv?'saved':'ram')}
  if(!ip4(ip))return 'bad ip';
  if(md=='server'){mk='255.255.255.0';ip=ip.replace(/\.\d+$/,'.1')}
  else if(!ip4(mk)||!maskok(mk))return 'bad mask';
  D.info.net.mode=md;D.info.net.ip=ip;D.info.net.mask=mk;
  D.info.net.ram=sv?0:1;
  if(sv)D.info.net.saved={mode:md,ip:ip,mask:mk};
  return 'ok '+md+' '+ip+' '+mk+(sv?' saved':' ram')}
 if(u=='/check')return JSON.stringify({items:check()});
 /* 样本页：全片均匀挑 48 页读得通的；带 chk= 时按预览条选的结果答 */
 if(u=='/sfmt')return JSON.stringify({pages:SFPG});
 if(u.indexOf('/sfmt?')==0){var q=u.slice(6),pm=/(?:^|&)(?:p|try|save|dd)=([0-9,]+)/.exec(q),
  pv=pm?pm[1].split(',').map(Number):null,
  ok=pv&&pv.slice(0,5).join(',')==SFTRUE&&S.dev=='pnand'&&S.sf!='none',
  mk=function(src,sv){return {st:'ok',ecc:pv[0],spare:pv[1],inv:pv[2],fdm:pv[3],fecc:pv[4],swap:pv[5],n:48,k:0,src:src,saved:sv}};
  if(q=='probe=1'){if(S.dev!='pnand'||S.sf=='na')return JSON.stringify({st:'na'});
   if(S.sf=='none')return JSON.stringify({st:'none'});
   if(S.sf=='amb')return JSON.stringify({st:'amb',k:2});
   var f0=JSON.parse(JSON.stringify(D.pnand.sfmt));SFCUR=f0;return JSON.stringify({st:'ok',sfmt:f0})}
  if(q.indexOf('try=')==0)return JSON.stringify(ok?{n:48,ok:48,fix:3}:{n:48,ok:0,fix:0});
  /* 没有 UBI（原厂那次）只记在内存里，重建之后才存得进去 */
  /* 存的就是设备现有那份：依据不变；改过的算手动填写 */
  if(q.indexOf('save=')==0){var cur=info().sfmt,same=cur&&cur.ecc&&[cur.ecc,cur.spare,cur.inv,cur.fdm,cur.fecc,cur.swap==null?0:cur.swap].join(',')==pv.join(',');
   SFCUR=mk(same?cur.src:'manual',S.dev=='pnand'?0:1);if(same)SFCUR.swap=cur.swap;
   return JSON.stringify({sfmt:SFCUR})}
  /* 一批最多 8 页；每页按参数解出来比 crc32，桩只看参数对不对、预览条选了什么 */
  if(q.indexOf('chk=')>=0){var ls=q.slice(q.indexOf('chk=')+4).split(',').map(function(x){return parseInt(x.split(':')[0],16)}),
   good=pv&&pv.slice(0,5).join(',')==SFTRUE&&S.dd!='bad',
   bd=ls.filter(function(o){return !good||(S.dd=='part'&&SFMOVED.indexOf(o)>=0)});
   return JSON.stringify({n:ls.length,s0:ls.length-bd.length,s1:0,bad:bd})}
  if(q.indexOf('dd=')==0){var nm=/&n=(\d+)/.exec(q);SFCUR=mk('dd',S.dev=='pnand'?0:1);SFCUR.n=nm?+nm[1]:48;
   return JSON.stringify({sfmt:SFCUR})}}
 /* 一段 4 MiB，和设备的 SCAN_SLICE 一样；坏块与 ECC 是编的，但位置固定 */
 if(u.indexOf('/scan')==0){
  var sz=D.info.flash.size,blk=D.info.flash.erase,sl=4<<20,
      m=/off=0x([0-9a-f]+)/.exec(u),a=m?parseInt(m[1],16):0,
      b=Math.min(sz,a+sl),bad=[],fail=[],ecc=0,x;
  for(x=a;x<b;x+=blk){
   if(x==0x2a00000||x==0x9c00000)bad.push(x);
   else if(x>=0xe000000&&((x/blk)%37)==0)ecc++}
  return JSON.stringify({off:b,size:sz,blk:blk,done:b>=sz?1:0,
   bad:bad.length,ecc:ecc,fail:fail.length,badlist:bad,faillist:fail})}
 if(u.indexOf('/wr')==0){var wf=/from=(\d+)/.exec(u);
  wf=wf?+wf[1]:0;if(wf>WRLOG.length)wf=WRLOG.length;
  return String(WRLOG.length)+'\n'+WRLOG.slice(wf)}
 if(u=='/log')return S.dev=='nolog'?null:D.log+LOGX;
 /* ?from= 只回新的那一段，第一行是新偏移 —— 和设备一样 */
 if(u.indexOf('/log?from=')==0){if(S.dev=='nolog')return null;
  var all=D.log+LOGX,f=parseInt(u.slice(10),10)||0;
  if(f>all.length)f=all.length;
  return String(all.length)+'\n'+all.slice(f)}
 if(u=='/env')return JSON.stringify({env:D.env,cut:0});
 if(u=='/envreset')return S.dev=='noubi'?'ok':'ok saved';
 if(u=='/wipecfg'){var ub=D.info.ubi;if(ub){ub.vols=ub.vols.filter(function(v){
  if(v.n=='rootfs_data'){ub.avail+=Math.floor(v.s/ub.leb);return false}return true})}
  return 'ok removed'}
 if(u.indexOf('/dhcpgw')==0){D.info.net.dgw=/on=1/.test(u)?1:0;return S.dev=='noubi'?'ok':'ok saved'}
 /*
  * 强制挂载，照真设备会怎样：混合状态卷表读得出，擦掉读不出的块就挂上了；原厂
  * 布局也挂得上，但原厂的 UBI 里没有 fip；整片别的格式连卷表都读不出，挂不上
  */
 if(u=='/ubiforce'){var to=S.dev=='ubiecc'?'ok':S.dev=='ubiforeign'?'nofip':'';
  if(!to)return 'attaching failed';
  S.dev=to;var pv=document.getElementById('pvdev');if(pv)pv.value=to;return 'ok attached'}
 if(u=='/bootonce')return S.dev=='noubi'?'armed, but saving failed':'armed and saved';
 if(u=='/boot'){fall(9000);setTimeout(function(){T0=Date.now()},9000);return 'ok'}
 if(u=='/reboot'){fall(9000);setTimeout(function(){T0=Date.now()},9000);return 'OK'}
 return null}
function XHR(){var x=this;x.upload={};x.status=0;x.responseText='';x.timeout=0;
x.open=function(m,u){x.m=m;x.u=u};x.setRequestHeader=function(){};
x.send=function(fd){
 if(x.m=='GET'){
  /* 回复超出缓冲区时设备给的是 JSON 500，解析得出来，但不是 /info */
  if(x.u=='/info'&&S.dev=='info500'&&!down()){setTimeout(function(){x.status=500;
   x.responseText='{"err":"reply too large"}';x.onload&&x.onload()},300);return}
  if(down()&&x.u!='/reboot'){setTimeout(function(){x.status=0;
   (x.timeout&&x.ontimeout?x.ontimeout:x.onerror||function(){})()},Math.min(x.timeout||1200,900));return}
  var t=body(x.u);
  setTimeout(function(){if(t==null){x.status=404;x.responseText='';x.onerror?x.onerror():x.onload&&x.onload()}
   else{x.status=200;x.responseText=t;x.onload&&x.onload()}},x.u=='/check'?1500:x.u=='/ping'?60:300);return}
 /* p4 发的是裸 File，不是 FormData —— 那条路没有表单可遍历 */
 var tot=0,n=0,st=!!(x.u&&x.u.indexOf('/stock')==0),parts=[],tryb=false,fmt=false;
 try{tot=fd.size||0}catch(e){}
 if(!tot)try{fd.forEach(function(v,k){if(v&&v.size){tot+=v.size;parts.push({k:k,n:v.size})}})}catch(e){}
 if(!tot)tot=1;
 try{tryb=fd.get('tryboot')=='1';fmt=fd.get('format')=='1'}catch(e){}
 var tick=setInterval(function(){n+=Math.max(tot/40,65536);if(n>=tot){n=tot;clearInterval(tick);x.upload.onprogress&&x.upload.onprogress({lengthComputable:true,loaded:n,total:tot});x.upload.onload&&x.upload.onload();
  setTimeout(function(){if(S.post=='drop'){x.onerror&&x.onerror();return}
   if(S.post=='reject'){x.status=400;x.responseText='the flash has no U-Boot (no fip volume) and this upload brings none: nothing would boot after the reset. Upload the U-Boot FIP as well';x.onload&&x.onload();return}
   /* 另一个上传占着时，设备把这个 POST 当 GET 答：200，整张页面 —— 真的那张，
      连脚本在内，页面据以判断的字眼它里面都有 */
   if(S.post=='busy'){x.status=200;x.responseText='<!DOCTYPE html>'+document.documentElement.outerHTML;x.onload&&x.onload();return}
   /* 刷回原厂仍是一次性回复：它边收边写，200 到手时早写完了 */
   if(st){if(S.post=='fail500'){x.status=500;
     x.responseText='写入 0x8c0000 失败（-5，实际写入 0/131072）。闪存已写入一部分，此时重启将无法启动。请重新写入至成功，其间不要断电'}
    else{x.status=200;
     x.responseText='ok '+tot+' bytes crc32 '+((0x3f2a91c4+tot)>>>0).toString(16)+' skipped 0'+wiped(x.u,tot);
     fall(60000)}
    x.onload&&x.onload();return}
   /* 试运行一去不回，所以它还是先回复后动手 */
   if(tryb){x.status=200;x.responseText='{"ok":1}';fall(9000);x.onload&&x.onload();return}
   /* 设备在 ACK 时就清了上一次的写入记录，回 200 之前：页面拿到 200 立刻去读 /wr，
      先清再答，否则读到的是上一次的 */
   WRLOG='';x.status=200;x.responseText='{"ok":1}';x.onload&&x.onload();
   wrrun(wrlines(parts,fmt),0);return},1200);return}
  x.upload.onprogress&&x.upload.onprogress({lengthComputable:true,loaded:n,total:tot})},80)}}
/*
 * A1 的行协议。写那一步报不出中间态（配方在 run_command 里），所以只有一句
 * 「正在写…」；回读校验是设备自己的循环，一段一段报得出来。
 */
var VNAME={bl2:'BL2',fip:'U-Boot',firmware:'固件',ubifile:'卷'};
function vname(p){return VNAME[p.k]||(p.k.indexOf('fvol_')==0?p.k.slice(5):p.k)}
function wrlines(parts,fmt){var L=[],tot=0,vd=Math.round(520/Math.max(1,parts.length));
 if(fmt)L.push(['s 重建 UBI',2600]);
 parts.forEach(function(p){var nm=vname(p);
  tot+=p.n;
  L.push(['s 写入 '+nm+' '+p.n,Math.max(800,Math.min(7000,p.n/1400000*1000))]);
  L.push(['r '+nm+' '+p.n+' '+((0x3f2a91c4+p.n)>>>0).toString(16),250])});
 if(S.post=='fail500'){L=L.slice(0,fmt?2:1);
  L.push(['f 写入 BL2 失败，详见串口日志',0]);
  return L}
 /* 和设备一样一个部分一段：「s 回读校验 <名> <字节>」，再一串 v；vbad 让最后一个部分不过 */
 for(var j=0;j<parts.length;j++){var q=parts[j],qn=vname(q);
  L.push(['s 回读校验 '+qn+' '+q.n,j?120:500]);
  for(var i=1;i<=5;i++)L.push(['v '+Math.round(q.n*i/5)+' '+q.n,vd]);
  if(S.post=='vbad'&&j==parts.length-1){L.push(['c bad '+qn+' 读回来的内容与上传的不一致',0]);return L}}
 L.push(['c ok',250]);
 L.push(['t '+tot+' '+Math.max(0.1,tot/1400000).toFixed(1),120]);
 L.push(['done',0]);
 return L}
/*
 * 写那一步真设备是不应答的（run_command 里出不来），所以在轮到 r 行之前
 * 让假设备也闭嘴同样长的时间 —— 页面要面对的正是这个。
 */
var WRLOG='';
function wrrun(L,i){if(i>=L.length)return;
 var d=L[i][1];
 WRLOG+=L[i][0]+'\n';
 if(L[i+1]&&L[i+1][0].charAt(0)=='r')fall(d);
 setTimeout(function(){wrrun(L,i+1)},d)}
window.XMLHttpRequest=XHR;
/* 桩的数据自己也是可看可改的：用例要模拟「网线换了个口」就动这里 */
window.PV=D;
document.addEventListener('DOMContentLoaded',function(){
 var b=document.createElement('div');
 /* 这条是预览自己的，不属于设备那张页面：别让语言切换去动它 */
 b.id='pvbar';b.setAttribute('data-raw','');
 b.setAttribute('style','position:fixed;right:12px;bottom:12px;z-index:99;background:#1d1d1f;color:#f5f5f7;font:12px/1.4 -apple-system,BlinkMacSystemFont,"PingFang SC","Microsoft YaHei",sans-serif;padding:8px 10px;border-radius:8px;box-shadow:0 8px 24px rgba(0,0,0,.35);display:flex;gap:8px;align-items:center;flex-wrap:wrap;max-width:calc(100vw - 24px)');
 b.innerHTML='<b>预览</b> 设备 <select id=pvdev><option value=ok>正常</option><option value=noubi>没有 UBI</option><option value=ubiecc>UBI 被拒：一部分是别的 ECC 格式</option><option value=ubieccall>UBI 被拒：整片别的 ECC 格式</option><option value=ubiforeign>UBI 被拒：原厂数据</option><option value=nofip>没有 fip 卷</option><option value=nolog>不带串口日志</option><option value=noinfo>/info 失败</option><option value=info500>/info 回 500</option><option value=pnand>并口 NAND 首次迁移前（HG5382A）</option><option value=pnandmig>并口 NAND 已迁移（HG5382A）</option></select> 原厂格式 <select id=pvsf><option value=ok>已识别</option><option value=amb>两种都读得通</option><option value=none>原厂不用 SoC ECC</option><option value=na>闪存已不是原厂内容</option></select> dd 核对 <select id=pvdd><option value=ok>一致</option><option value=part>两块被挪过</option><option value=bad>全对不上</option></select> 提交 <select id=pvpost><option value=ok>成功</option><option value=reject>设备拒绝 400</option><option value=fail500>写到一半失败 500</option><option value=drop>断线</option><option value=busy>另一个上传占着</option><option value=vbad>回读校验没通过</option></select> 连接 <select id=pvconn><option value=up>正常</option><option value=down>断开</option></select>';
 document.body.appendChild(b);
 var sel=b.querySelector('#pvdev'),ps=b.querySelector('#pvpost'),cn=b.querySelector('#pvconn'),sf=b.querySelector('#pvsf'),dd=b.querySelector('#pvdd');
 sf.onchange=function(){S.sf=sf.value;SFCUR=null;window.info&&window.info()};
 dd.onchange=function(){S.dd=dd.value};
 sel.onchange=function(){S.dev=sel.value;SFCUR=null;if(window.CHK!==undefined)window.CHK=null;window.ENV=null;window.info&&window.info()};
 ps.onchange=function(){S.post=ps.value};
 cn.onchange=function(){S.conn=cn.value};
 /*
  * 下载本身在 file:// 下没有设备可下，所以只换掉最里面这一层：读取期间设备
  * 静默、读完记下 crc32 —— 页面那套问 /dumpinfo 的逻辑跑的是真的。
  *
  * 「静默」是整段传输，不是象征性的一下子。net/tcp.c 只有一个
  * static struct tcp_stream，旧连接没 CLOSED 之前新 SYN 直接被拒，而下载那条
  * 从头开到尾 —— 所以这期间设备对任何请求都不应答。桩以前只 fall(900)，于是
  * /dumpinfo 一路答得好好的，页面那套百分比、速率、剩余时间在这里全绿，在真
  * 设备上一次都没出现过。判据自己错了，比没有判据更坏。
  */
 window.dlstart=function(u,n){
  /* 长度留空时由设备算到片尾，这里照做，好让进度和 crc32 都有个数 */
  var fl=info().flash;
  if(n===null){var o=/off=0x([0-9a-f]+)/.exec(u);
   n=fl.size-(o?parseInt(o[1],16):0);
   if(/oob=1/.test(u))n=n/fl.page*(fl.page+fl.oob)}
  /* 第一个窗口读完就开始传，所以静默很短；crc32 要等整份传完才有 */
  var send=Math.max(1200,Math.min(n/1e4,30000)),t0=Date.now();
  fall(send);
  DBUSY=1;DSENT=0;DTOTAL=n;clearInterval(DTICK);
  DTICK=setInterval(function(){
   DSENT=Math.min(n,Math.round(n*(Date.now()-t0)/send))},200);
  setTimeout(function(){DSEQ++;clearInterval(DTICK);
   DBUSY=0;DSENT=n;
   DINFO={seq:DSEQ,len:n,crc:(0x3f2a91c4+DSEQ*7).toString(16),holes:0,
          name:info().model.toLowerCase().replace(/[^a-z0-9]+/g,'-')+'-'+(/vol=([^&]+)/.exec(u)||[0,'flash'])[1]+(/oob=1/.test(u)?'-oob':'')+'.bin'}},
   send)};
});
})();</script>
"""


def render(html, stock=True, pnand=True):
    # Markers nest (PNAND sits inside STOCK on the restore page), so keep a
    # stack.  PNAND is in unless --no-pnand: with it the stub decides whether
    # the board has parallel NAND, the way /info does on a real one; without
    # it the page is what an SPI NAND build serves.
    out = []
    keep = [True]
    for line in html.splitlines():
        s = line.strip()
        if s == "<!--#if STOCK-->":
            keep.append(keep[-1] and stock)
            continue
        if s == "<!--#if PNAND-->":
            keep.append(keep[-1] and pnand)
            continue
        if s == "<!--#endif-->":
            keep.pop()
            continue
        if not keep[-1]:
            continue
        out.append(line)
    html = "\n".join(out) + "\n"
    for k, v in MACROS.items():
        html = html.replace("@@%s@@" % k, v)
    data = json.dumps({"info": INFO, "check": CHECK, "log": LOG,
                       "pnand": PNAND_INFO,
                       "env": [{"k": k, "v": v} for k, v in ENV]},
                      ensure_ascii=False)
    stub = STUB.replace("@DATA@", data)
    return html.replace("</head>", stub + "</head>", 1)


def main():
    args = [a for a in sys.argv[1:]
            if a not in ("--no-stock", "--no-pnand", "--raw")]
    stock = "--no-stock" not in sys.argv[1:]
    pnand = "--no-pnand" not in sys.argv[1:]
    html = open(args[0], encoding="utf-8").read()
    if "--raw" not in sys.argv[1:]:
        html = minify(html)
    # argv[2] is written over, so it is the destination and never the source.
    dst = args[1] if len(args) > 1 else "preview.html"
    open(dst, "w", encoding="utf-8", newline="").write(render(html, stock, pnand))
    print("wrote", dst, "(no stock)" if not stock else "",
          "(no pnand)" if not pnand else "")


if __name__ == "__main__":
    main()
