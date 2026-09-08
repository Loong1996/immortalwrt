#!/usr/bin/env python3
"""Render page.html as a standalone preview with a fake device behind it.

Usage: preview.py page.html [preview.html]

Opens in any browser straight from disk: every request the page makes
(/info, /check, /log, the POST) is answered by a stub, and a small bar at
the bottom right switches the device between the states the page has to
handle -- healthy, no UBI, no fip volume, a build without console
recording, /info failing -- and picks how a submit ends.  Look at the
page here before touching the C side; what the stub answers is what the
real endpoints answer, so a layout or wording change is decided on the
same data.

Markers are handled the way gen.py does, except that the STOCK section is
always shown and the @@MACROS@@ get sample values.
"""
import json
import sys

MACROS = {
    "WEB_VERSION": "0.3.0",
    "PROJECT_URL": "https://github.com/Loong1996/ImmortalWrt-Airoha",
    "PROJECT_HOST": "github.com/Loong1996/ImmortalWrt-Airoha",
    "PORTAL_URL": "https://loong1996.github.io/ImmortalWrt-Airoha/",
    "PORTAL_HOST": "loong1996.github.io/ImmortalWrt-Airoha",
}

VOLS = [
    {"i": 0, "n": "fip", "t": "static", "s": 1015808, "u": 325632},
    {"i": 1, "n": "fit", "t": "dynamic", "s": 13000704, "u": 12984320},
    {"i": 2, "n": "ubootenv", "t": "dynamic", "s": 126976, "u": 126976},
    {"i": 3, "n": "ubootenv2", "t": "dynamic", "s": 126976, "u": 126976},
    {"i": 4, "n": "bosa", "t": "dynamic", "s": 380928, "u": 262144},
    {"i": 5, "n": "ri", "t": "dynamic", "s": 380928, "u": 262144},
    {"i": 6, "n": "rootfs_data", "t": "dynamic", "s": 242862080, "u": 0},
]

INFO = {
    "web": MACROS["WEB_VERSION"],
    "model": "Nokia XG-040G-MD",
    "soc": "airoha,an7581",
    "ram": 536870912,
    "mac": "90:03:2e:12:34:56",
    "uboot": "U-Boot 2026.07-ImmortalWrt (Sep 06 2026 - 10:21:03 +0800)",
    "flash": {"name": "spi-nand0", "size": 268435456, "erase": 131072,
              "page": 2048},
    "parts": [{"n": "bl2", "o": 0, "s": 131072},
              {"n": "ubi", "o": 131072, "s": 268304384}],
    "stock": 1,
    "log": 1,
    "fv": [{"n": "ri", "s": 262144}, {"n": "bosa", "s": 262144}],
    "ubi": {"leb": 126976, "pebs": 2046, "fip": 1, "vols": VOLS},
}

CHECK = [
    ["闪存", 0, "spi-nand0，256 MiB，擦除块 128 KiB"],
    ["坏块", 0, "没有"],
    ["BL2", 0, "0x800 处有 BL2 镜像"],
    ["UBI", 0, "7 个卷，坏块 0 个，空闲 1836 个逻辑擦除块"],
    ["fip 卷", 0, "325632 字节，校验通过"],
    ["fit 卷", 0, "FIT 镜像，12984320 字节"],
    ["ubootenv 卷", 0, "存在"],
    ["ubootenv2 卷", 0, "存在"],
    ["ri 卷", 0, "读到了，MAC 90:03:2e:12:34:56"],
    ["bosa 卷", 1, "读到了，内容为空"],
    ["U-Boot MAC", 0, "90:03:2e:12:34:56"],
]

LOG = """

U-Boot 2026.07-ImmortalWrt-r40957-4b007b8c20 (Sep 05 2026 - 17:01:01 +0000)

CPU:   Airoha AN7581
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
Airoha Web U-Boot %s by Loong
Using airoha-gdm1 device, MAC 90:03:2e:12:34:56
Listening for HTTP on 192.168.1.1 port 80
Handing out DHCP leases from 192.168.1.1
Press Ctrl-C to abort
httpd: DHCP OFFER -> 192.168.1.100
httpd: DHCP ACK -> 192.168.1.100
""" % MACROS["WEB_VERSION"]

# The stub.  Plain ES5 like the page itself.
STUB = r"""
<script>(function(){
var D=@DATA@,S={dev:'ok',post:'ok'};
function info(){var i=JSON.parse(JSON.stringify(D.info));
if(S.dev=='noubi')i.ubi=null;
if(S.dev=='nofip'){i.ubi.fip=0;i.ubi.vols=i.ubi.vols.filter(function(v){return v.n!='fip'})}
if(S.dev=='nolog')i.log=0;
return i}
function check(){var c=D.check.map(function(r){return{n:r[0],s:r[1],v:r[2]}});
if(S.dev=='noubi')return c.slice(0,3).concat([{n:'UBI',s:2,v:'无法挂载：闪存上没有可用的 UBI。首次迁移请在「引导升级」里打开「重建 UBI」，并同时上传 BL2、U-Boot 与固件'},c[c.length-1]]);
if(S.dev=='nofip')c[4]={n:'fip 卷',s:2,v:'不存在：现在运行的 U-Boot 只在内存里，请到「引导升级」里上传 U-Boot 文件'};
return c}
function XHR(){var x=this;x.upload={};x.status=0;x.responseText='';
x.open=function(m,u){x.m=m;x.u=u};x.setRequestHeader=function(){};
x.send=function(fd){
 if(x.m=='GET'){var t=x.u=='/info'?(S.dev=='noinfo'?null:JSON.stringify(info())):x.u=='/check'?JSON.stringify({items:check()}):x.u=='/log'?(S.dev=='nolog'?null:D.log):null;
  setTimeout(function(){if(t==null){x.status=x.u=='/info'&&S.dev=='noinfo'?0:404;x.responseText='';(x.status?x.onload:x.onerror)&&(x.status?x.onload():x.onerror())}else{x.status=200;x.responseText=t;x.onload&&x.onload()}},x.u=='/check'?1500:300);return}
 var tot=0,n=0;try{fd.forEach(function(v){if(v&&v.size)tot+=v.size})}catch(e){}if(!tot)tot=1;
 var tick=setInterval(function(){n+=Math.max(tot/40,65536);if(n>=tot){n=tot;clearInterval(tick);x.upload.onprogress&&x.upload.onprogress({lengthComputable:true,loaded:n,total:tot});x.upload.onload&&x.upload.onload();
  setTimeout(function(){if(S.post=='drop'){x.onerror&&x.onerror();return}
   if(S.post=='reject'){x.status=400;x.responseText='the flash has no U-Boot (no fip volume) and this upload brings none: nothing would boot after the reset. Upload the U-Boot FIP as well'}else{x.status=200;x.responseText='OK'}
   x.onload&&x.onload()},1200);return}
  x.upload.onprogress&&x.upload.onprogress({lengthComputable:true,loaded:n,total:tot})},80)}}
window.XMLHttpRequest=XHR;
document.addEventListener('DOMContentLoaded',function(){
 var b=document.createElement('div');
 b.setAttribute('style','position:fixed;right:12px;bottom:12px;z-index:99;background:#1d1d1f;color:#f5f5f7;font:12px/1.4 -apple-system,BlinkMacSystemFont,"PingFang SC","Microsoft YaHei",sans-serif;padding:8px 10px;border-radius:8px;box-shadow:0 8px 24px rgba(0,0,0,.35);display:flex;gap:8px;align-items:center');
 b.innerHTML='<b>预览</b> 设备 <select id=pvdev><option value=ok>正常</option><option value=noubi>没有 UBI</option><option value=nofip>没有 fip 卷</option><option value=nolog>不带串口日志</option><option value=noinfo>/info 失败</option></select> 提交 <select id=pvpost><option value=ok>成功</option><option value=reject>设备拒绝 400</option><option value=drop>断线</option></select>';
 document.body.appendChild(b);
 var sel=b.querySelector('#pvdev'),ps=b.querySelector('#pvpost');
 sel.onchange=function(){S.dev=sel.value;if(window.CHK!==undefined)window.CHK=null;window.info&&window.info()};
 ps.onchange=function(){S.post=ps.value};
});
})();</script>
"""


def render(html):
    out = []
    for line in html.splitlines():
        s = line.strip()
        if s in ("<!--#if STOCK-->", "<!--#endif-->"):
            continue
        out.append(line)
    html = "\n".join(out) + "\n"
    for k, v in MACROS.items():
        html = html.replace("@@%s@@" % k, v)
    data = json.dumps({"info": INFO, "check": CHECK, "log": LOG},
                      ensure_ascii=False)
    stub = STUB.replace("@DATA@", data)
    return html.replace("</head>", stub + "</head>", 1)


def main():
    html = open(sys.argv[1], encoding="utf-8").read()
    dst = sys.argv[2] if len(sys.argv) > 2 else "preview.html"
    open(dst, "w", encoding="utf-8", newline="").write(render(html))
    print("wrote", dst)


if __name__ == "__main__":
    main()
