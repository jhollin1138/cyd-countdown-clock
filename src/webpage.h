// Settings page served at http://countdown.local/
#pragma once
#include <Arduino.h>

static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Countdown Clock</title>
<style>
:root{--bg:#0f172a;--card:#1e293b;--fg:#e2e8f0;--mut:#94a3b8;--acc:#f59e0b;--bd:#334155;--in:#0b1220}
*{box-sizing:border-box}body{margin:0;font-family:system-ui,-apple-system,Segoe UI,sans-serif;background:var(--bg);color:var(--fg)}
header{padding:16px;background:linear-gradient(90deg,#0ea5e9,#f59e0b);color:#0f172a}
h1{margin:0;font-size:20px}#st{font-size:13px;margin-top:4px}
main{max-width:760px;margin:auto;padding:12px}
section{background:var(--card);border:1px solid var(--bd);border-radius:12px;padding:14px;margin:12px 0}
h2{margin:0 0 8px;font-size:16px;color:var(--acc)}
label{display:block;font-size:13px;color:var(--mut);margin:10px 0 4px}
input,select{width:100%;padding:9px;border-radius:8px;border:1px solid var(--bd);background:var(--in);color:var(--fg);font-size:15px}
input[type=checkbox]{width:18px;height:18px}input[type=range]{padding:0}
.row{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.ev{border:1px solid var(--bd);border-radius:10px;padding:10px 12px;margin:10px 0}
button{background:var(--acc);color:#111;border:0;border-radius:8px;padding:9px 14px;font-weight:600;font-size:14px;cursor:pointer}
button.sec{background:#334155;color:var(--fg)}button.del{background:#b91c1c;color:#fff}
.imgs{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:10px;margin-top:10px}
.imgs div{font-size:12px;text-align:center;word-break:break-all}.imgs img{width:100%;border-radius:6px;aspect-ratio:4/3;object-fit:cover;display:block;margin-bottom:4px}
.chk{display:flex;align-items:center;gap:8px;margin-top:10px;font-size:14px}
.mut{color:var(--mut);font-size:13px}
.snd{display:flex;align-items:center;gap:8px;padding:8px 0;border-bottom:1px solid var(--bd);font-size:14px}.snd span{flex:1;word-break:break-all}
textarea{width:100%;padding:9px;border-radius:8px;border:1px solid var(--bd);background:var(--in);color:var(--fg);font:13px ui-monospace,monospace;min-height:70px}
progress{width:100%;height:16px;margin-top:8px}
.bar{position:sticky;bottom:0;background:linear-gradient(transparent,var(--bg) 30%);padding:16px 0 10px;text-align:right}
#toast{position:fixed;bottom:70px;left:50%;transform:translateX(-50%);background:#16a34a;color:#fff;padding:10px 16px;border-radius:8px;display:none;z-index:9}
@media(max-width:540px){.row{grid-template-columns:1fr}}
</style></head><body>
<header><h1>&#9203; Countdown Clock</h1><div id="st">Loading&hellip;</div></header>
<main>
<section><h2>Events</h2><p class="mut">Events are kept in date order, soonest first.</p><div id="evs"></div><button class="sec" onclick="addEv()">+ Add event</button></section>

<section><h2>Background photos</h2>
<p class="mut">Photos are cropped and resized to 320&times;240 on this device before upload. Storage: <span id="stor"></span></p>
<div class="row"><div><label>Crop focus</label><select id="focus"><option value="0.5">Center</option><option value="0.15">Top</option><option value="0.85">Bottom</option></select></div>
<div><label>Add photos</label><input type="file" id="file" accept="image/*" multiple></div></div>
<div id="up" class="mut"></div><div class="imgs" id="imgs"></div></section>

<section><h2>Clock</h2>
<div class="row"><div><label>Home time zone</label><select id="tzsel"></select><input id="tz" style="margin-top:6px" placeholder="POSIX TZ string"></div>
<div><label>Units</label><select id="metric"><option value="0">&deg;F, mph</option><option value="1">&deg;C, km/h</option></select></div></div>
<label>Home location (weather on the home clock screen)</label>
<div style="display:flex;gap:6px"><input id="qh" placeholder="City name" onkeydown="if(event.key=='Enter')geoH()"><button class="sec" onclick="geoH()">Search</button></div>
<select id="gh" style="display:none;margin-top:6px" onchange="pickH(this.value)"></select>
<div class="mut" id="llh" style="margin-top:4px"></div>
<div class="chk"><input type="checkbox" id="h24"><span>24-hour clock</span></div>
<div class="row"><div><label>With several events</label><select id="mode"><option value="nearest">Show the next event</option><option value="rotate">Rotate between events</option></select></div>
<div><label>Rotate every (seconds)</label><input type="number" id="rotateSec" min="5" max="3600"></div></div>
<p class="mut">On the clock: swipe or tap to switch events &middot; tap the weather for the 5-day forecast &middot; tap the time (top right) for the home clock &middot; hold 1 second for the menu (brightness, volume, events, WiFi, settings) &middot; hold 8 seconds to recalibrate touch.</p></section>

<section><h2>Display</h2>
<div class="chk"><input type="checkbox" id="autoBright"><span>Auto-brightness from the light sensor</span></div>
<div class="row">
<div><label>Manual brightness: <b id="mbv"></b></label><input type="range" id="manualBright" min="5" max="255" oninput="$('mbv').textContent=this.value"></div><div></div>
<div><label>Auto minimum</label><input type="range" id="minBright" min="0" max="255"></div>
<div><label>Auto maximum</label><input type="range" id="maxBright" min="0" max="255"></div>
<div><label>Sensor reading, bright room</label><input type="number" id="ldrBright" min="0" max="4095"></div>
<div><label>Sensor reading, dark room</label><input type="number" id="ldrDark" min="0" max="4095"></div></div>
<p class="mut">Live sensor reading: <b id="ldr">&ndash;</b>&nbsp; <button class="sec" onclick="$('ldrBright').value=ldrNow">Use as bright</button> <button class="sec" onclick="$('ldrDark').value=ldrNow">Use as dark</button></p>
<div class="chk"><input type="checkbox" id="invert"><span>Invert colors (some board versions need this)</span></div>
<div class="chk"><input type="checkbox" id="flip"><span>Rotate screen 180&deg;</span></div></section>

<section><h2>Quiet hours</h2>
<div class="chk"><input type="checkbox" id="quietOn"><span>Dim the screen and silence chimes overnight</span></div>
<div class="row"><div><label>Start</label><input type="time" id="quietStart"></div><div><label>End</label><input type="time" id="quietEnd"></div></div>
<label>Brightness during quiet hours (0 = off; tap the screen to wake it for 30 s)</label><input type="range" id="quietLevel" min="0" max="100"></section>

<section><h2>Sound &amp; milestones</h2>
<p class="mut">A sound and LED show at 100, 60, 30, 14, 7, 3, 2, 1 and 0 days, and a celebration when the countdown ends.</p>
<div class="chk"><input type="checkbox" id="sound"><span>Sound on</span></div>
<label>Volume: <b id="volv"></b></label><input type="range" id="volume" min="0" max="100" step="5" oninput="$('volv').textContent=this.value+'%'" onchange="preview()">
<div class="row"><div><label>Milestone sound</label><select id="soundMile"></select></div>
<div><label>Celebration sound</label><select id="soundArr"></select></div></div>
<div class="chk"><input type="checkbox" id="led"><span>RGB LED effects</span></div>
<p><button class="sec" onclick="post('/api/test?w=0')">Test milestone</button> <button class="sec" onclick="post('/api/test?w=1')">Test celebration</button> <button class="sec" onclick="post('/api/sound/stop')">Stop sound</button></p>
<h2 style="margin-top:18px">Sound library</h2>
<p class="mut">Storage: <span id="sstor"></span>. Short clips work best (under 30 seconds; the clock stops anything after 90 s). Mono 22 kHz MP3s are small and sound the same on this speaker.</p>
<div id="snds"></div>
<label>Add an MP3 or WAV file</label><input type="file" id="sfile" accept=".mp3,.wav,audio/mpeg,audio/wav">
<div id="sup" class="mut"></div>
<label>Or paste a ringtone (RTTTL), e.g. <code>Tune:d=4,o=5,b=120:c,e,g,2c6</code></label>
<textarea id="rt" placeholder="name:d=4,o=5,b=100:notes..."></textarea>
<p style="text-align:right;margin:6px 0 0"><button class="sec" onclick="addRt()">Add ringtone</button></p></section>

<section><h2>Backup &amp; restore</h2>
<p class="mut">Saves your events, locations and settings (not photos or sounds) to a file on this device.</p>
<p><a href="/api/config?download=1" download="countdown-backup.json"><button class="sec">Download backup</button></a></p>
<label>Restore from a backup file</label><input type="file" id="rfile"></section>

<section><h2>Firmware update</h2>
<p class="mut">Installed: <b id="fwv">&ndash;</b>. After building in VS Code, choose the versioned file in the project's <code>firmware</code> folder (e.g. <code>countdown-clock-cyd-v1.3.0.bin</code>), or a release download. Your settings, photos and sounds are kept.</p>
<input type="file" id="fw" accept=".bin"><progress id="fwp" max="100" value="0" style="display:none"></progress><div id="fws" class="mut"></div>
<p style="text-align:right;margin:6px 0 0"><button onclick="flash()">Install update</button></p></section>

<section><h2>System</h2><div id="sys" class="mut"></div>
<p><button class="sec" onclick="if(confirm('Reboot the clock?'))post('/api/reboot')">Reboot</button>
<button class="del" onclick="if(confirm('Forget WiFi and restart in setup mode?'))post('/api/wifireset')">Reset WiFi</button></p></section>

<div class="bar"><button onclick="save()">Save settings</button></div>
<p class="mut" style="text-align:center;margin:20px 0 70px">Countdown Clock by jhollin1138</p></main><div id="toast"></div>
<script>
const $=id=>document.getElementById(id);
let C={events:[]},IM=[],ldrNow=0,GR={};
const TZ=[["US Eastern","EST5EDT,M3.2.0,M11.1.0"],["US Central","CST6CDT,M3.2.0,M11.1.0"],["US Mountain","MST7MDT,M3.2.0,M11.1.0"],["Arizona","MST7"],["US Pacific","PST8PDT,M3.2.0,M11.1.0"],["Alaska","AKST9AKDT,M3.2.0,M11.1.0"],["Hawaii","HST10"],["UK / Ireland","GMT0BST,M3.5.0/1,M10.5.0"],["Central Europe","CET-1CEST,M3.5.0,M10.5.0/3"],["Custom (type below)",""]];
const F=['tz','metric','h24','mode','rotateSec','autoBright','manualBright','minBright','maxBright','ldrBright','ldrDark','invert','flip','quietOn','quietStart','quietEnd','quietLevel','sound','volume','soundMile','soundArr','led'];
let SB=[],SF=[];
function toast(m,bad){const t=$('toast');t.textContent=m;t.style.background=bad?'#b91c1c':'#16a34a';t.style.display='block';clearTimeout(t._h);t._h=setTimeout(()=>t.style.display='none',2800)}
async function post(u){try{const r=await fetch(u,{method:'POST'});r.ok?toast('Done'):toast(await r.text(),1)}catch(e){toast('Clock not reachable',1)}}
function esc(s){return String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
function fill(){
  $('tzsel').innerHTML=TZ.map(t=>`<option value="${t[1]}">${t[0]}</option>`).join('');
  $('tzsel').onchange=function(){if(this.value)$('tz').value=this.value};
  sndOpts();
  F.forEach(k=>{const e=$(k);if(e.type=='checkbox')e.checked=!!C[k];else if(k=='metric')e.value=C[k]?'1':'0';else e.value=C[k]??''});
  $('tzsel').value=TZ.some(t=>t[1]==C.tz)?C.tz:'';$('mbv').textContent=C.manualBright;$('volv').textContent=C.volume+'%';$('qh').value=C.homeName||'';showH();renderEvs()}
function showH(){$('llh').innerHTML=C.homeLat!=null?'&#127968; '+(+C.homeLat).toFixed(3)+', '+(+C.homeLon).toFixed(3):'Not set'}
async function geoH(){const q=$('qh').value.trim();if(!q)return;
  try{const r=await(await fetch('https://geocoding-api.open-meteo.com/v1/search?count=8&language=en&format=json&name='+encodeURIComponent(q))).json();
  GR.h=r.results||[];const s=$('gh');if(!GR.h.length){s.style.display='none';return toast('No matches found',1)}
  s.innerHTML='<option value="">Choose a match&hellip;</option>'+GR.h.map((g,k)=>`<option value="${k}">${esc([g.name,g.admin1,g.country].filter(Boolean).join(', '))}</option>`).join('');s.style.display='block'}
  catch(e){toast('Search needs internet access from this device',1)}}
function pickH(k){if(k==='')return;const g=GR.h[k];C.homeName=[g.name,g.admin1].filter(Boolean).join(', ');C.homeLat=g.latitude;C.homeLon=g.longitude;$('qh').value=C.homeName;$('gh').style.display='none';showH()}
function gather(){F.forEach(k=>{const e=$(k);if(e.type=='checkbox')C[k]=e.checked;else if(k=='metric')C[k]=e.value=='1';else if(e.type=='number'||e.type=='range')C[k]=+e.value;else C[k]=e.value})}
function imgOpts(sel){let o=`<option value="">Built-in sunset</option>`;IM.forEach(n=>o+=`<option${sel==n?' selected':''}>${esc(n)}</option>`);
  if(sel&&!IM.includes(sel))o+=`<option value="${esc(sel)}" selected>${esc(sel)} (missing - upload it again)</option>`;return o}
const SHADES=['None','Light','Medium','Dark'];
function shadeOpts(v){v=v??1;return SHADES.map((n,k)=>`<option value="${k}"${k==v?' selected':''}>${n}</option>`).join('')}
function renderEvs(){
  $('evs').innerHTML=C.events.map((e,i)=>`<div class="ev"><div class="row">
<div><label>Event name</label><input value="${esc(e.name)}" maxlength="32" onchange="C.events[${i}].name=this.value"></div>
<div><label>Count down to (home time, e.g. departure)</label><input type="datetime-local" value="${esc(e.target)}" onchange="C.events[${i}].target=this.value"></div></div>
<label>Location (weather, local time, sunrise/sunset)</label>
<div style="display:flex;gap:6px"><input id="q${i}" value="${esc(e.place)}" placeholder="City name" onkeydown="if(event.key=='Enter')geo(${i})"><button class="sec" onclick="geo(${i})">Search</button></div>
<select id="g${i}" style="display:none;margin-top:6px" onchange="pick(${i},this.value)"></select>
<div class="mut" id="ll${i}" style="margin-top:4px">${e.lat!=null?'&#128205; '+(+e.lat).toFixed(3)+', '+(+e.lon).toFixed(3):'No location set yet'}</div>
<div class="row"><div><label>Background</label><select onchange="C.events[${i}].img=this.value">${imgOpts(e.img)}</select></div>
<div><label>Shading behind the text</label><select onchange="C.events[${i}].shade=+this.value">${shadeOpts(e.shade)}</select></div></div>
<p style="text-align:right;margin:10px 0 0"><button class="del" onclick="if(confirm('Remove this event?')){C.events.splice(${i},1);renderEvs()}">Remove</button></p></div>`).join('')||'<p class="mut">No events yet. Add one below.</p>'}
function addEv(){if(C.events.length>=6)return toast('Maximum of 6 events',1);C.events.push({name:'New event',target:'',place:'',img:'',shade:1});renderEvs()}
async function geo(i){const q=$('q'+i).value.trim();if(!q)return;
  try{const r=await(await fetch('https://geocoding-api.open-meteo.com/v1/search?count=8&language=en&format=json&name='+encodeURIComponent(q))).json();
  GR[i]=r.results||[];const s=$('g'+i);if(!GR[i].length){s.style.display='none';return toast('No matches found',1)}
  s.innerHTML='<option value="">Choose a match&hellip;</option>'+GR[i].map((g,k)=>`<option value="${k}">${esc([g.name,g.admin1,g.country].filter(Boolean).join(', '))}</option>`).join('');s.style.display='block'}
  catch(e){toast('Search needs internet access from this device',1)}}
function pick(i,k){if(k==='')return;const g=GR[i][k],e=C.events[i];e.place=[g.name,g.admin1].filter(Boolean).join(', ');e.lat=g.latitude;e.lon=g.longitude;
  $('q'+i).value=e.place;$('ll'+i).innerHTML='&#128205; '+g.latitude.toFixed(3)+', '+g.longitude.toFixed(3)+' ('+esc(g.country||'')+')';$('g'+i).style.display='none'}
async function loadImgs(){const r=await(await fetch('/api/images')).json();IM=r.files||[];$('stor').textContent=`${r.storage}, ${Math.floor(r.free/1024)} KB free`;
  $('imgs').innerHTML=IM.map(n=>`<div><img loading="lazy" src="/img?name=${encodeURIComponent(n)}">${esc(n)}<br><button class="del" style="margin-top:4px" data-n="${esc(n)}" onclick="delImg(this.dataset.n)">Delete</button></div>`).join('')||'<p class="mut">No photos yet.</p>'}
async function delImg(n){if(!confirm('Delete '+n+'?'))return;await post('/api/image/delete?name='+encodeURIComponent(n));await loadImgs();C.events.forEach(e=>{if(e.img==n)e.img=''});renderEvs()}
function safe(n){return(n.replace(/\.[^.]*$/,'').toLowerCase().replace(/[^a-z0-9_-]+/g,'-').replace(/^-+|-+$/g,'').slice(0,24)||'photo')+'.jpg'}
function resize(file){return new Promise((ok,bad)=>{const img=new Image();img.onload=()=>{const W=320,H=240,c=document.createElement('canvas');c.width=W;c.height=H;const x=c.getContext('2d');
  const s=Math.max(W/img.width,H/img.height),w=img.width*s,h=img.height*s;x.imageSmoothingQuality='high';x.drawImage(img,(W-w)/2,(H-h)*(+$('focus').value),w,h);URL.revokeObjectURL(img.src);
  c.toBlob(b=>b?ok(b):bad('could not encode'),'image/jpeg',0.85)};img.onerror=()=>bad('not a supported image');img.src=URL.createObjectURL(file)})}
$('file').onchange=async e=>{let n=0;for(const f of e.target.files){$('up').textContent='Uploading '+f.name+'…';
  try{const b=await resize(f);const fd=new FormData();fd.append('f',b,safe(f.name));const r=await fetch('/api/upload',{method:'POST',body:fd});if(!r.ok)throw await r.text();n++}catch(x){toast(f.name+': '+x,1)}}
  $('up').textContent='';e.target.value='';await loadImgs();renderEvs();if(n)toast(n+' photo'+(n>1?'s':'')+' uploaded')};
function sndLabel(id){const b=SB.find(x=>x.id==id);return b?b.label:id.replace(/\.[^.]*$/,'')}
function sndOpts(){const all=SB.map(b=>b.id).concat(SF.map(f=>f.name));
  ['soundMile','soundArr'].forEach(k=>{const cur=C[k];let ids=all.slice();if(cur&&!ids.includes(cur))ids.push(cur);
    $(k).innerHTML=ids.map(id=>`<option value="${esc(id)}">${esc(sndLabel(id))}</option>`).join('');$(k).value=cur||''})}
async function loadSnds(){try{const r=await(await fetch('/api/sounds')).json();SB=r.builtins||[];SF=r.files||[];
  $('sstor').textContent=`${r.storage}, ${Math.floor(r.free/1024)} KB free`;
  $('snds').innerHTML=SB.map(b=>`<div class="snd"><span>${esc(b.label)}</span><button class="sec" data-n="${esc(b.id)}" onclick="play(this.dataset.n)">&#9654; Play</button></div>`).join('')+
    SF.map(f=>`<div class="snd"><span>${esc(f.name)} <span class="mut">(${Math.ceil(f.size/1024)} KB)</span></span><button class="sec" data-n="${esc(f.name)}" onclick="play(this.dataset.n)">&#9654; Play</button><button class="del" data-n="${esc(f.name)}" onclick="delSnd(this.dataset.n)">Delete</button></div>`).join('');
  if(C.soundMile!==undefined){gather();sndOpts()}}catch(e){}}
function play(n){post('/api/sound/play?name='+encodeURIComponent(n)+'&vol='+$('volume').value)}
function preview(){play($('soundMile').value)}
async function delSnd(n){if(!confirm('Delete '+n+'?'))return;await post('/api/sound/delete?name='+encodeURIComponent(n));
  ['soundMile','soundArr'].forEach(k=>{if(C[k]==n)C[k]=k=='soundMile'?'builtin:chime':'builtin:fanfare'});await loadSnds()}
$('sfile').onchange=async e=>{const f=e.target.files[0];if(!f)return;$('sup').textContent='Uploading '+f.name+'…';
  try{const fd=new FormData();fd.append('f',f,f.name);const r=await fetch('/api/sound/upload',{method:'POST',body:fd});if(!r.ok)throw await r.text();toast('Sound added')}catch(x){toast(f.name+': '+x,1)}
  $('sup').textContent='';e.target.value='';await loadSnds()};
async function addRt(){const t=$('rt').value.trim();if(!t)return;
  try{const r=await fetch('/api/sound/rtttl',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({text:t})});if(!r.ok)throw await r.text();$('rt').value='';toast('Ringtone added')}catch(x){toast(String(x),1)}
  await loadSnds()}
$('rfile').onchange=async e=>{const f=e.target.files[0];if(!f)return;e.target.value='';
  try{const b=JSON.parse(await f.text());if(!b||!Array.isArray(b.events))throw 'not a countdown backup';
    if(!confirm('Replace all current settings and events with this backup?'))return;
    C=b;fill();await save();await loadSnds()}catch(x){toast('Restore failed: '+x,1)}};
function flash(){const f=$('fw').files[0];if(!f)return toast('Choose firmware.bin first',1);
  if(!confirm('Install '+f.name+' (currently running '+$('fwv').textContent+')? The clock restarts when it finishes.'))return;
  const x=new XMLHttpRequest(),fd=new FormData(),p=$('fwp');fd.append('firmware',f,f.name);p.style.display='block';p.value=0;$('fws').textContent='Uploading…';
  x.upload.onprogress=ev=>{if(ev.lengthComputable)p.value=Math.round(ev.loaded*100/ev.total)};
  x.onload=()=>{if(x.status==200){$('fws').textContent='Installed. Restarting…';setTimeout(waitBack,6000)}else{$('fws').textContent='';toast(x.responseText||'Update failed',1);p.style.display='none'}};
  x.onerror=()=>{$('fws').textContent='';toast('Upload failed',1);p.style.display='none'};
  x.open('POST','/api/firmware');x.send(fd)}
async function waitBack(){for(let i=0;i<30;i++){try{const r=await fetch('/api/status',{cache:'no-store'});if(r.ok){location.reload();return}}catch(e){}await new Promise(r=>setTimeout(r,2000))}
  $('fws').textContent='The clock hasn\'t come back yet. Check its screen.'}
async function status(){try{const s=await(await fetch('/api/status')).json();ldrNow=s.ldr;$('ldr').textContent=s.ldr;
  $('st').textContent=s.time+(s.active?' · showing: '+s.active:'');
  $('fwv').textContent='v'+s.fw;
  $('sys').innerHTML=`Firmware ${esc(s.fw)} &middot; IP ${esc(s.ip)} &middot; WiFi ${esc(s.ssid)} (${s.rssi} dBm) &middot; Free memory ${Math.floor(s.heap/1024)} KB &middot; Up ${Math.floor(s.up/3600)}h ${Math.floor(s.up%3600/60)}m`}catch(e){$('st').textContent='Clock not reachable'}}
async function save(){gather();for(const e of C.events){if(!e.target)return toast('Set a date and time for "'+e.name+'"',1)}
  try{const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(C)});if(!r.ok)return toast(await r.text(),1);toast('Saved');
    C=await(await fetch('/api/config')).json();C.events=C.events||[];renderEvs()}catch(e){toast('Clock not reachable',1)}}
(async()=>{try{await loadImgs();await loadSnds();C=await(await fetch('/api/config')).json();C.events=C.events||[];fill()}catch(e){toast('Could not load settings',1)}status();setInterval(status,2000)})();
</script></body></html>)HTML";
