// Airoha Web U-Boot -- cases for ../page.html.
//
// The page is the part of this project a user actually touches, and the one
// part that cannot be checked by compiling.  So it is driven here instead:
// preview.py renders exactly the HTML the device serves, with a stub device
// answering every request the page makes, and these cases drive that document
// in jsdom.  The stub answers what the real endpoints answer, so a case that
// passes here is a case about the real page -- not about a copy of it.
//
//     cd package/boot/uboot-airoha/files/httpd/test
//     npm install && npm test
//
// Needs python3 for preview.py; set PYTHON= to override the interpreter name.
const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');
const { JSDOM, VirtualConsole } = require('jsdom');

const HERE = __dirname;
const HTML = path.join(HERE, 'preview.html');
const PY = process.env.PYTHON ||
	(process.platform === 'win32' ? 'python' : 'python3');

// Render first, always: a stale preview.html would test yesterday's page.
execFileSync(PY, [path.join(HERE, '..', 'preview.py'),
		  path.join(HERE, '..', 'page.html'), HTML], { stdio: 'inherit' });
let pass = 0, fail = 0;

function ok(name, cond, extra) {
  if (cond) { pass++; console.log('  ok   ' + name); }
  else { fail++; console.log('  FAIL ' + name + (extra ? '  <' + extra + '>' : '')); }
}
const sleep = ms => new Promise(r => setTimeout(r, ms));

async function boot() {
  // jsdom cannot navigate, so a location.reload() surfaces as a jsdomError;
  // that is how the tests observe whether the page decided to reload.
  const errs = [];
  const vc = new VirtualConsole();
  vc.on('jsdomError', e => errs.push(String(e && e.message)));
  const dom = new JSDOM(fs.readFileSync(HTML, 'utf8'), {
    url: 'http://192.168.1.1/', runScripts: 'dangerously', pretendToBeVisual: true,
    virtualConsole: vc,
  });
  const w = dom.window;
  w.__errs = errs;
  await new Promise(r => w.addEventListener('load', r));
  await sleep(600);
  return w;
}
const $ = (w, s) => w.document.querySelector(s);
const txt = (w, s) => ($(w, s) || {}).textContent || '';
const on = (w, s) => $(w, s).hasAttribute('data-on');
function setsel(w, id, v) { const s = $(w, '#pv' + id); s.value = v; s.onchange(); }

function stayUpload(w) {
  $(w, '.nav[data-p=p3]').click();
  const f = $(w, '#p3 input[name=ubifile]');
  Object.defineProperty(f, 'files', { value: [new w.File([new Uint8Array(1024)], 'ri.bin')] });
  $(w, '#p3 input[name=ubivol]').value = 'ri';
  $(w, '#p3 input[name=stay]').checked = true;
  w.send();
}

(async () => {
  console.log('\n--- 备份下载 (p10) ---');
  {
    const w = await boot();
    ok('侧栏有备份下载入口', !!$(w, '.nav[data-p=p10]'));
    $(w, '.nav[data-p=p10]').click();
    ok('点进去切到 p10', on(w, '#p10'));

    const rows = [...w.document.querySelectorAll('#dl tr')];
    ok('七个卷都列出来了', rows.length === 7, rows.length);
    ok('出厂数据排在最前', /^(ri|bosa)/.test(rows[0].cells[0].textContent) &&
                          /^(ri|bosa)/.test(rows[1].cells[0].textContent),
       rows.map(r => r.cells[0].textContent).join(','));
    ok('出厂卷带标记', !!rows[0].querySelector('.tag') && !rows[2].querySelector('.tag'));
    ok('大小取已用而非预留',
       rows.find(r => /^fit/.test(r.cells[0].textContent)).cells[2].textContent === '12.4 MiB');

    let url = null;
    w.sink = (u, what, n) => { url = u; };
    rows.find(r => /^ri/.test(r.cells[0].textContent)).querySelector('button').click();
    ok('卷下载走 /dump?vol=', url === '/dump?vol=ri', url);

    w.dumpall();
    ok('整片下载不带 len，交给设备去数', url === '/dump?off=0x0', url);

    $(w, '#dumpoff').value = '0x20000'; $(w, '#dumplen').value = '';
    w.dumpraw();
    ok('留空长度也不带 len', url === '/dump?off=0x20000', url);

    url = null;
    $(w, '#dumpoff').value = '0x0'; $(w, '#dumplen').value = '0x20000000';
    w.dumpraw();
    ok('超出可读容量不发请求',
       url === null && /最多只读得出/.test(txt(w, '#dlh')), txt(w, '#dlh'));
    $(w, '#dumpoff').value = 'zz'; w.dumpraw();
    ok('偏移不是十六进制不发请求', url === null && /十六进制/.test(txt(w, '#dlh')));
    ok('长度提示报的是可读字节数',
       txt(w, '#dumphint').includes(w.sz(w.INFO.flash.good)) &&
       /坏块/.test(txt(w, '#dumphint')), txt(w, '#dumphint'));

    /* 有坏块的机器：能读出来的比标称容量少，上限得跟着它走 */
    const good0 = w.INFO.flash.good;
    w.INFO.flash.good = good0 - 0x40000;        /* 两个坏块 */
    url = null;
    $(w, '#dumpoff').value = '0x0';
    $(w, '#dumplen').value = '0x' + good0.toString(16);
    w.dumpraw();
    ok('坏块吃掉的那部分要不到',
       url === null && /最多只读得出/.test(txt(w, '#dlh')), txt(w, '#dlh'));
    $(w, '#dumplen').value = '0x' + (good0 - 0x40000).toString(16);
    w.dumpraw();
    ok('刚好可读的长度放行',
       url === '/dump?off=0x0&len=0x' + (good0 - 0x40000).toString(16), url);
    w.INFO.flash.good = good0;
    ok('p10 上回车不弹写入确认框', w.ask() === false && !on(w, '#mask'));
  }

  console.log('\n--- 没有 UBI 时的备份下载 ---');
  {
    const w = await boot();
    setsel(w, 'dev', 'noubi');
    await sleep(600);
    $(w, '.nav[data-p=p10]').click();
    ok('提示改用原始区段', /原始区段/.test(txt(w, '#dl')));
  }

  console.log('\n--- 环境变量 (p11) ---');
  {
    const w = await boot();
    ok('侧栏有环境变量入口', !!$(w, '.nav[data-p=p11]'));
    $(w, '.nav[data-p=p11]').click();
    await sleep(600);
    let rows = [...w.document.querySelectorAll('#envt tr')];
    ok('把 env 全列出来', rows.length === 27, rows.length);
    ok('计数对得上', /27 \/ 27 项/.test(txt(w, '#envh')), txt(w, '#envh'));

    $(w, '#envq').value = 'bootmenu'; w.envfill();
    ok('按名称过滤', w.document.querySelectorAll('#envt tr').length === 2);
    $(w, '#envq').value = 'ubi remove'; w.envfill();
    rows = [...w.document.querySelectorAll('#envt tr')];
    ok('按值也能过滤', rows.length === 1 &&
       /ubi_write_production/.test(rows[0].cells[0].textContent));
    $(w, '#envq').value = '没有这个'; w.envfill();
    ok('没有匹配时说清楚', /没有匹配的变量/.test(txt(w, '#envt')));

    $(w, '#envq').value = ''; $(w, '#envkey').checked = true; w.envfill();
    const names = [...w.document.querySelectorAll('#envt tr')].map(r => r.cells[0].textContent);
    ok('只看关键项筛掉噪声', names.length === 18 && !names.includes('stdin'), names.length);
    ok('关键项留下 bootcmd 与 envver',
       names.includes('bootcmd') && names.includes('envver'));

    $(w, '#envkey').checked = false; w.envfill();
    w.askenvdef();
    ok('恢复默认弹确认框', on(w, '#mask'));
    ok('确认框写明命令', /env default -a && saveenv/.test(txt(w, '#abody')));
    ok('确认框说清出厂 MAC 不受影响', /出厂 MAC/.test(txt(w, '#abody')));
    ok('主按钮改成恢复默认', txt(w, '#yes') === '恢复默认');
    $(w, '#mask button.pb:not(.red)').click();
    ok('取消就什么都不做', !on(w, '#mask') && w.YES === null);

    w.askenvdef();
    $(w, '#yes').click();
    await sleep(600);
    ok('恢复成功说重启后生效', /已恢复并保存，重启后生效/.test(txt(w, '#envh')), txt(w, '#envh'));
  }

  console.log('\n--- UBI 挂不上时恢复默认环境 ---');
  {
    const w = await boot();
    setsel(w, 'dev', 'noubi');
    await sleep(600);
    $(w, '.nav[data-p=p11]').click();
    await sleep(600);
    w.askenvdef();
    $(w, '#yes').click();
    await sleep(600);
    ok('保存失败要说断电即失', /保存失败.*断电即失/.test(txt(w, '#envh')), txt(w, '#envh'));
  }

  console.log('\n--- 心跳与断开 ---');
  {
    const w = await boot();
    ok('侧栏显示已连接', txt(w, '#lives') === '已连接', txt(w, '#lives'));
    ok('连接标识在侧栏底部', !!$(w, '.side .foot #lived'));
    ok('点是绿的', $(w, '#lived').className === 'dot s0', $(w, '#lived').className);
    ok('一切正常时没有覆盖层', !on(w, '#off'));

    setsel(w, 'conn', 'down');
    await sleep(4500);                       // 心跳 3s 一次，失败判定 0.9s
    ok('掉一次只转黄不弹框', !on(w, '#off') && $(w, '#lived').className === 'dot s1',
       $(w, '#lived').className);
    await sleep(4500);                       // 第二次失败才算断开
    ok('连掉两次才弹框', on(w, '#off'));
    ok('说的是已断开', /已断开与路由器连接/.test(txt(w, '#offt')), txt(w, '#offt'));
    ok('给了排查线索', /网线|重启|写入/.test(txt(w, '#offb')));
    ok('有重新连接按钮', !$(w, '#offr').hidden);
    ok('点变红', $(w, '#lived').className === 'dot s2');

    $(w, '#offr').click();
    ok('重连按钮进入重试态', $(w, '#offr').disabled);

    setsel(w, 'conn', 'up');
    await sleep(4000);
    ok('设备回来覆盖层自己消失', !on(w, '#off'));
    ok('回来之后点转绿', $(w, '#lived').className === 'dot s0');
    ok('同一次上电不刷新页面', !w.__errs.some(e => /navigation/i.test(e)),
       w.__errs.join('|'));
  }

  console.log('\n--- 自己发起的读取不打扰 ---');
  {
    const w = await boot();
    $(w, '.nav[data-p=p10]').click();
    w.dumpall();
    await sleep(5000);
    ok('整片下载期间不弹框', !on(w, '#off'));
    ok('边读边传，点一直是绿的', $(w, '#lived').className === 'dot s0',
       $(w, '#lived').className);
    ok('传完记了一行', w.document.querySelectorAll('#dll tr').length === 1);

    // 真正拖住设备的是体检那种全片扫描：那时只变点，不盖框
    w.silent();
    setsel(w, 'conn', 'down');
    await sleep(9000);
    ok('长时间静默也不弹框', !on(w, '#off'));
    ok('点旁边说设备忙', /设备忙/.test(txt(w, '#lives')), txt(w, '#lives'));

    setsel(w, 'conn', 'up');
    await sleep(4000);
    ok('回来就恢复已连接', txt(w, '#lives') === '已连接', txt(w, '#lives'));
    ok('全程没弹过框', !on(w, '#off'));
  }

  console.log('\n--- 体检期间也不弹框 ---');
  {
    const w = await boot();
    $(w, '.nav[data-p=p8]').click();
    await sleep(2200);
    ok('体检跑完且没弹框', !on(w, '#off') && /15 项正常/.test(txt(w, '#chkh')));
  }

  console.log('\n--- 重启 (p12) ---');
  {
    const w = await boot();
    ok('侧栏有重启入口', !!$(w, '.nav[data-p=p12]'));
    $(w, '.nav[data-p=p12]').click();
    ok('点进去切到 p12', on(w, '#p12'));
    ok('说清闪存不受影响', /不改动闪存/.test(txt(w, '#p12')));
    ok('重启页不啰嗦', txt(w, '#p12').replace(/\s/g, '').length < 100,
       txt(w, '#p12').replace(/\s/g, '').length);

    $(w, '#p12 button.red').click();
    ok('重启要确认', on(w, '#mask') && /reset/.test(txt(w, '#abody')));
    ok('主按钮是立即重启', txt(w, '#yes') === '立即重启');
    $(w, '#yes').click();
    await sleep(300);
    ok('立刻说设备在重启', on(w, '#off') && /设备正在重启/.test(txt(w, '#offt')), txt(w, '#offt'));
    ok('重启时不给重连按钮', $(w, '#offr').hidden);
    ok('并说会自动刷新', /自动刷新/.test(txt(w, '#offb')));
    ok('重启提示不啰嗦', txt(w, '#offb').replace(/\s/g, '').length < 40,
       txt(w, '#offb').replace(/\s/g, '').length);
  }

  console.log('\n--- 「写入后不重启」靠心跳回报 ---');
  {
    const w = await boot();
    stayUpload(w);
    ok('上传期间不发心跳', w.HBOFF === 1);
    await sleep(3000);
    ok('上传完立刻说设备在写', on(w, '#off') && /设备正在写入/.test(txt(w, '#offt')), txt(w, '#offt'));
    ok('并且不让人点重连', $(w, '#offr').hidden);
    ok('写入期间提醒别断电', /不要断电|请勿断电/.test(txt(w, '#offb')));
    await sleep(9000);                       // 桩在 200 之后哑 7 秒
    ok('设备回来覆盖层消失', !on(w, '#off'), txt(w, '#offt'));
    ok('进度条改成写完了', /设备写完了/.test(txt(w, '#p3 .pwhat')), txt(w, '#p3 .pwhat'));
    ok('进度条转绿', $(w, '#p3 .prog').className === 'prog ok');
  }

  console.log('\n--- 会重启的那种提交不弹断开框 ---');
  {
    const w = await boot();
    $(w, '.nav[data-p=p1]').click();
    const f = $(w, '#p1 input[name=firmware]');
    Object.defineProperty(f, 'files', { value: [new w.File([new Uint8Array(2048)], 'x.itb')] });
    w.send();
    await sleep(3000);
    ok('走到上传完成页', on(w, '#p7'));
    await sleep(5000);
    ok('上传完成页上不弹断开框', !on(w, '#off'));
    ok('心跳已经停了', w.HB === 0);
  }

  console.log('\n--- 备份完之后能核对 ---');
  {
    const w = await boot();
    $(w, '.nav[data-p=p10]').click();
    [...w.document.querySelectorAll('#dl tr')]
      .find(r => /^ri/.test(r.cells[0].textContent))
      .querySelector('button').click();
    await sleep(700);
    ok('先说正在读并传送', /正在读并传送 ri 卷/.test(txt(w, '#dlh')), txt(w, '#dlh'));
    ok('读的时候还没有 crc', !/crc32/.test(txt(w, '#dlh')));
    await sleep(4500);
    ok('传完出现一行记录', w.document.querySelectorAll('#dll tr').length === 1);
    const row = $(w, '#dll tr');
    ok('记录带文件名', /\.bin$/.test(row.cells[0].textContent), row.cells[0].textContent);
    ok('记录带长度', row.cells[1].textContent === '256 KiB', row.cells[1].textContent);
    ok('记录带 crc32', /^[0-9a-f]+$/.test(row.cells[2].textContent), row.cells[2].textContent);
    ok('说清这个数怎么用', /本地核对/.test(txt(w, '#dllh')));
    ok('说清多大都是一个文件', /都是一个文件/.test(txt(w, '#p10')));
    ok('读取期间没弹断开框', !on(w, '#off'));

    // 分段存档的人需要每一段的 crc，新的不能盖掉旧的
    [...w.document.querySelectorAll('#dl tr')]
      .find(r => /^bosa/.test(r.cells[0].textContent))
      .querySelector('button').click();
    await sleep(5200);
    const rows = [...w.document.querySelectorAll('#dll tr')];
    ok('第二段往下排而不是覆盖', rows.length === 2, rows.length);
    ok('两段的 crc 不一样', rows[0].cells[2].textContent !== rows[1].cells[2].textContent);
  }

  console.log('\n--- 整片就是一个文件 ---');
  {
    const w = await boot();
    $(w, '.nav[data-p=p10]').click();
    let url = null;
    w.dlstart = u => { url = u; };

    w.dumpall();
    await sleep(600);
    ok('整片一次要到底', url === '/dump?off=0x0', url);
    ok('没有「下一段」这种东西', !$(w, '#dlnext'));
    ok('长度未知时说的是到片尾', /到片尾/.test(txt(w, '#dlh')), txt(w, '#dlh'));

    $(w, '#dumpoff').value = '0x20000'; $(w, '#dumplen').value = '';
    w.dumpraw();
    await sleep(600);
    ok('留空长度读到片尾', url === '/dump?off=0x20000', url);

    url = null;
    $(w, '#dumpoff').value = '0x0';
    $(w, '#dumplen').value = '0x' + (w.INFO.flash.size + 0x20000).toString(16);
    w.dumpraw();
    ok('还是拦得住超出容量的', url === null &&
       /最多只读得出/.test(txt(w, '#dlh')), txt(w, '#dlh'));
  }

  console.log('\n--- 设备拒绝时不用干等四分钟 ---');
  {
    /*
     * 下载走隐藏 iframe：真下载会被浏览器接走、不触发 load，只有错误页会。
     * 这里直接喂给 dlrefused()，不必让 jsdom 真去导航一个 iframe。
     */
    const w = await boot();
    $(w, '.nav[data-p=p10]').click();
    w.dlstart = () => {};
    w.dumpall();
    await sleep(50);
    ok('先报在读', /正在读/.test(txt(w, '#dlh')), txt(w, '#dlh'));

    w.dlrefused({ contentDocument: { body: { textContent: '  \n ' } } });
    ok('空白的 load 不当成错误',
       w.DLBAD === 0 && /正在读/.test(txt(w, '#dlh')), txt(w, '#dlh'));

    w.dlrefused({ contentDocument: { body: { textContent:
      ' 从 0x0 起只读得出 268173312 字节（已扣掉坏块），要不了 268435456\n' } } });
    ok('拒绝的理由当场显示', /只读得出/.test(txt(w, '#dlh')), txt(w, '#dlh'));
    ok('理由里的换行被压平', !/\n/.test(txt(w, '#dlh')));
    ok('不再等 /dumpinfo', w.DLBAD === 1);

    const before = txt(w, '#dlh');
    await sleep(2400);
    ok('轮询确实停了', txt(w, '#dlh') === before, txt(w, '#dlh'));
  }

  console.log('\n--- 太大的镜像不用传就知道 ---');
  {
    const w = await boot();
    const max = w.INFO.uploadmax;
    ok('设备报了上传上限', max > 0, max);
    $(w, '.nav[data-p=p4]').click();
    ok('「刷回原厂」页上写着上限', txt(w, '#upmax') === w.sz(max), txt(w, '#upmax'));

    const pick = (n) => {
      const i = $(w, '#p4 input[name=stock]');
      const f = new w.File([new Uint8Array(1)], 'all_flash.bin');
      Object.defineProperty(f, 'size', { value: n });
      Object.defineProperty(i, 'files', { value: [f], configurable: true });
    };

    pick(max + 1024);
    w.ask();
    ok('超了就弹框', on(w, '#mask'));
    ok('说清超了多少', /超过设备一次能收下的/.test(txt(w, '#abody')), txt(w, '#abody'));
    ok('这是硬错误，不给「仍要写入」', $(w, '#yes').hidden);
    w.hide();

    // 原厂 all_flash 是 235.6 MiB，本来就在上限之内 —— 别把它也拦了
    pick(0xEBA0000);
    w.ask();
    ok('原厂镜像照样放行', !$(w, '#yes').hidden);
    ok('也没有多余的报错', !/超过设备一次能收下的/.test(txt(w, '#abody')));
    w.hide();
  }

  console.log('\n--- 健康检查分组 ---');
  {
    const w = await boot();
    $(w, '.nav[data-p=p8]').click();
    await sleep(2200);
    const heads = [...w.document.querySelectorAll('#chk th.g')].map(t => t.textContent);
    ok('五组标题都在', heads.join(',') === '闪存,引导,UBI,环境,出厂数据', heads.join(','));
    ok('十六项都在',
       w.document.querySelectorAll('#chk td.s0,#chk td.s1,#chk td.s2').length === 16);
    ok('envver 报出来了', /envver/.test(txt(w, '#chk')));
    ok('固件版本报出来了', /2026-09-05 17:01/.test(txt(w, '#chk')));
    ok('两份环境对比过', /与 ubootenv 一致/.test(txt(w, '#chk')));
    ok('汇总仍然正确', /1 项注意 · 15 项正常/.test(txt(w, '#chkh')), txt(w, '#chkh'));
  }

  console.log('\n--- 老功能没被碰坏 ---');
  {
    const w = await boot();
    ok('设备详情还在填', /Nokia XG-040G-MD/.test(txt(w, '#dev')));
    ok('UBI 卷表还在', w.document.querySelectorAll('#ubi tr').length === 8);
    $(w, '.nav[data-p=p9]').click();
    await sleep(600);
    ok('串口日志还能读', /Airoha Web U-Boot/.test(txt(w, '#log')));
    ok('日志里有内存推导过程', /probing by address aliasing/.test(txt(w, '#log')));
    ok('日志里有每一步的锚点读数', /anchor now 0xa5a55a5a/.test(txt(w, '#log')));
    setsel(w, 'dev', 'nofip');
    await sleep(600);
    ok('没有 fip 卷的红条还在', !$(w, '#ban').hasAttribute('hidden') &&
       /没有 U-Boot/.test(txt(w, '#bant')));
    ok('日常刷机页说清断电就退出', /断电重启就退出/.test(txt(w, '#p1')));

    const who = [...w.document.querySelectorAll('a')]
      .filter(a => a.textContent === 'Loong');
    ok('三处作者名都是链接', who.length === 3, who.length);
    ok('指向作者的 GitHub 主页',
       who.every(a => a.getAttribute('href') === 'https://github.com/Loong1996'),
       who.map(a => a.getAttribute('href')).join('|'));
    ok('新标签页打开且带 noopener',
       who.every(a => a.target === '_blank' && /noopener/.test(a.rel)));
    ok('侧栏副标题那处也在', !!$(w, '.side .id a'));

    // 页内跳转链接是内联 onclick，改错了函数名只有点下去才炸
    const dead = [...w.document.querySelectorAll('[onclick]')]
      .map(e => e.getAttribute('onclick'))
      .flatMap(h => [...h.matchAll(/([A-Za-z_$][\w$]*)\s*\(/g)].map(m => m[1]))
      .filter(fn => !['if', 'for', 'while', 'switch', 'catch', 'return',
                     'typeof', 'function', 'new'].includes(fn))
      .filter(fn => typeof w[fn] !== 'function');
    ok('所有内联 onclick 调的函数都存在', dead.length === 0, dead.join(','));
  }

  console.log('\n' + pass + ' passed, ' + fail + ' failed');
  process.exit(fail ? 1 : 0);
})();
