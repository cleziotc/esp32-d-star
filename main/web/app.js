const $=s=>document.querySelector(s), $$=s=>[...document.querySelectorAll(s)];
let statusCache={}, configCache={};
let currentReflectorType="XLX", hostPollTimer=null, updatePollTimer=null, updateAutoCheckStarted=false;
function applyVersion(v){const label='v'+v;const d=$('#dashFirmwareVersion');if(d)d.textContent=label;const o=$('#otaFirmwareVersion');if(o)o.textContent=label;const f=$('#footerVersion');if(f)f.textContent='Polar D-Star ESP32 '+label;}
function setLamp(id,on,kind){const el=$(id);if(!el)return;el.className='lamp '+(on?(kind||'rx'):'off');}
function toast(msg,error=false){const t=$('#toast');t.textContent=msg;t.className='toast show'+(error?' error':'');clearTimeout(window.__toast);window.__toast=setTimeout(()=>t.className='toast',2800)}
async function api(path,opt={}){const r=await fetch(path,{cache:'no-store',...opt});let j={};try{j=await r.json()}catch{}if(!r.ok)throw new Error(j.error||('HTTP '+r.status));return j}
function showPage(name){$$('.page').forEach(p=>p.classList.remove('active'));const p=$('#page-'+name)||$('#page-dashboard');p.classList.add('active');$$('.nav-btn').forEach(b=>b.classList.toggle('active',b.dataset.page===name));$('#mainNav').classList.remove('open');history.replaceState(null,'','#'+name);if(name==='logs')loadLogs();if(name==='system')loadSystem();if(name==='network'){loadWifi();loadHostsStatus();loadReflectors(currentReflectorType,$('#reflectorSelect')?.value||configCache.reflector)}if(name==='radio')drawChart();if(name==='update')loadOnlineUpdate()}
$$('[data-page]').forEach(b=>b.addEventListener('click',()=>showPage(b.dataset.page)));$('#menuBtn').onclick=()=>$('#mainNav').classList.toggle('open');
function fmtUptime(s){s=Number(s||0);const d=Math.floor(s/86400);s%=86400;const h=Math.floor(s/3600);s%=3600;const m=Math.floor(s/60);return (d?d+'d ':'')+h+'h '+m+'m'}
function fmtHz(hz){return (Number(hz||0)/1e6).toFixed(6)+' MHz'}
async function refreshStatus(){try{const s=await api('/api/status');statusCache=s;applyVersion(s.version);$('#topCall').textContent=s.callsign;$('#dashCall').textContent=s.callsign;$('#dashModule').textContent=s.module;$('#topState').textContent='ONLINE';$('#uptime').textContent='Uptime: '+fmtUptime(s.uptime_s);$('#rxFreq').textContent=fmtHz(s.rx_hz);$('#txFreq').textContent=fmtHz(s.tx_hz);$('#radioFreq').textContent=fmtHz(s.rx_hz);$('#reflectorTitle').textContent=s.reflector+' '+s.reflector_module;$('#mmdvmState').textContent=s.mmdvm_state;$('#wifiIp').textContent=s.ip;$('#wifiRssi').textContent=(s.rssi>-120?s.rssi+' dBm':'--');$('#rssiMini').textContent=(s.rssi>-120?s.rssi+' dBm':'AP local');$('#wifiState').textContent=s.wifi_connected?'CONECTADO':'AP local';setLamp('#txLamp',!!s.tx_active,'tx');setLamp('#rxLamp',!!s.rx_active,'rx')}catch(e){$('#topState').textContent='OFFLINE';setLamp('#txLamp',false);setLamp('#rxLamp',false)}}
async function loadConfig(){try{const c=await api('/api/config'),f=$('#dstarForm');configCache=c;f.callsign.value=c.callsign;f.module.value=c.module;f.location.value=c.location;f.rx_mhz.value=(c.rx_hz/1e6).toFixed(6);f.tx_mhz.value=(c.tx_hz/1e6).toFixed(6);f.rx_offset_khz.value=c.rx_offset_hz/1000;f.tx_offset_khz.value=c.tx_offset_hz/1000;f.tx_level.value=c.tx_level;f.rx_level.value=c.rx_level;currentReflectorType=c.reflector_type||((c.reflector||'').startsWith('XLX')?'XLX':(c.reflector||'').slice(0,3))||'XLX';setReflectorTab(currentReflectorType);buildModuleOptions(c.reflector_module||'D');await loadReflectors(currentReflectorType,c.reflector);syncForm()}catch(e){toast(e.message,true)}}
function syncForm(){const f=$('#dstarForm');$('#txLevelOut').textContent=f.tx_level.value;$('#rxLevelOut').textContent=f.rx_level.value;$('#rpt1Preview').textContent=(f.callsign.value||'NOCALL')+' '+f.module.value;$('#rpt2Preview').textContent=(f.callsign.value||'NOCALL')+' G'}
$('#dstarForm').addEventListener('input',syncForm);$('#dstarForm').addEventListener('submit',async e=>{e.preventDefault();const f=e.currentTarget;const body={callsign:f.callsign.value.trim().toUpperCase(),module:f.module.value,location:f.location.value.trim(),rx_hz:Math.round(Number(f.rx_mhz.value)*1e6),tx_hz:Math.round(Number(f.tx_mhz.value)*1e6),rx_offset_hz:Math.round(Number(f.rx_offset_khz.value)*1000),tx_offset_hz:Math.round(Number(f.tx_offset_khz.value)*1000),tx_level:Number(f.tx_level.value),rx_level:Number(f.rx_level.value),reflector:$('#reflectorSelect').value||configCache.reflector||'XLX300',reflector_type:currentReflectorType,reflector_module:$('#reflectorModule').value||'D'};try{await api('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});configCache={...configCache,...body};toast('Configuração salva');await refreshStatus()}catch(err){toast(err.message,true)}});
function esc(s){return String(s??'').replace(/[&<>"']/g,m=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[m]))}
function renderSavedNetworks(w){const box=$('#savedNetworks'),profiles=w.profiles||[];$('#wifiProfileCount').textContent=`${profiles.length}/${w.max_profiles||5} redes salvas`;box.innerHTML=profiles.length?profiles.map(p=>`<div class="wifi-item ${p.connected?'connected':''}"><div><b>${esc(p.ssid)}</b><small>${p.connected?'● conectada':'salva'}</small></div><button type="button" class="wifi-delete" data-ssid="${esc(p.ssid)}">Remover</button></div>`).join(''):'<div class="empty-mini">Nenhuma rede salva.</div>';$$('.wifi-delete').forEach(b=>b.onclick=()=>deleteWifi(b.dataset.ssid))}
async function loadWifi(){try{const w=await api('/api/wifi');$('#wifiDot').className='dot '+(w.connected?'ok':'');$('#wifiSummary').textContent=w.connected?`${w.ssid} • ${w.ip} • ${w.rssi} dBm`:`Somente AP de configuração • ${w.ap_ip}`;renderSavedNetworks(w)}catch(e){toast(e.message,true)}}
async function scanWifi(){const btn=$('#scanWifiBtn'),box=$('#wifiScanResults');btn.disabled=true;btn.textContent='Pesquisando...';box.innerHTML='<div class="empty-mini">Pesquisando redes Wi-Fi...</div>';try{const d=await api('/api/wifi/scan');const items=d.items||[];box.innerHTML=items.length?items.map(x=>`<button type="button" class="wifi-scan-item" data-ssid="${esc(x.ssid)}"><span><b>${esc(x.ssid)}</b><small>Canal ${x.channel} • ${x.open?'aberta':'protegida'}</small></span><strong>${x.rssi} dBm</strong></button>`).join(''):'<div class="empty-mini">Nenhuma rede encontrada.</div>';$$('.wifi-scan-item').forEach(b=>b.onclick=()=>{const f=$('#wifiForm');f.ssid.value=b.dataset.ssid;f.password.focus()})}catch(e){box.innerHTML='<div class="empty-mini">Falha ao pesquisar redes.</div>';toast(e.message,true)}finally{btn.disabled=false;btn.textContent='Pesquisar redes'}}
async function deleteWifi(ssid){if(!confirm(`Remover a rede salva “${ssid}”?`))return;try{await api('/api/wifi/delete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid})});toast('Rede removida');await loadWifi()}catch(e){toast(e.message,true)}}
$('#scanWifiBtn').onclick=scanWifi;
$('#wifiForm').addEventListener('submit',async e=>{e.preventDefault();const f=e.currentTarget;if(!f.ssid.value.trim()){toast('Informe o SSID',true);return}try{await api('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:f.ssid.value.trim(),password:f.password.value})});f.password.value='';toast('Rede salva; tentando conectar...');setTimeout(loadWifi,1800)}catch(err){toast(err.message,true)}});
function buildModuleOptions(selected='D'){const sel=$('#reflectorModule');sel.innerHTML='';for(let c=65;c<=90;c++){const ch=String.fromCharCode(c),o=document.createElement('option');o.value=ch;o.textContent=ch;if(ch===selected)o.selected=true;sel.appendChild(o)}}
function setReflectorTab(type){currentReflectorType=type;$$('[data-ref-type]').forEach(b=>b.classList.toggle('active',b.dataset.refType===type))}
async function loadReflectors(type=currentReflectorType,selected=''){setReflectorTab(type);const sel=$('#reflectorSelect');sel.disabled=true;sel.innerHTML='<option>Carregando lista...</option>';try{const d=await api('/api/reflectors?type='+encodeURIComponent(type));const items=d.items||[];sel.innerHTML='';if(selected&&!items.some(x=>x.name===selected)){const o=document.createElement('option');o.value=selected;o.textContent=selected+' (salvo)';sel.appendChild(o)}for(const x of items){const o=document.createElement('option');o.value=x.name;o.textContent=x.name;o.title=x.ip||'';if(x.name===selected)o.selected=true;sel.appendChild(o)}if(!sel.options.length){const o=document.createElement('option');o.value=selected||'';o.textContent=selected||'Lista ainda não sincronizada';sel.appendChild(o)}sel.disabled=false}catch(e){sel.innerHTML=`<option>${esc(selected||'Lista indisponível')}</option>`;toast(e.message,true)}}
async function loadHostsStatus(){try{const h=await api('/api/hosts');$('#hostSyncState').textContent=h.syncing?'Sincronizando...':`Última sincronização: ${h.last_sync}`;const c=h.counts||{};$('#hostCounts').textContent=`XLX ${c.XLX||0} • REF ${c.REF||0} • XRF ${c.XRF||0} • DCS ${c.DCS||0}`;$('#syncHostsBtn').disabled=!!h.syncing;if(h.syncing){clearTimeout(hostPollTimer);hostPollTimer=setTimeout(async()=>{await loadHostsStatus();if(!$('#syncHostsBtn').disabled)loadReflectors(currentReflectorType,$('#reflectorSelect').value||configCache.reflector)},1800)}}catch(e){$('#hostSyncState').textContent='Falha ao consultar hosts'}}
$('#syncHostsBtn').onclick=async()=>{try{await api('/api/hosts/sync',{method:'POST'});toast('Atualização de hosts solicitada');$('#syncHostsBtn').disabled=true;$('#hostSyncState').textContent='Sincronização agendada...';clearTimeout(hostPollTimer);hostPollTimer=setTimeout(loadHostsStatus,1000)}catch(e){toast(e.message,true)}};
$$('[data-ref-type]').forEach(b=>b.onclick=()=>loadReflectors(b.dataset.refType,''));
$('#saveReflectorBtn').onclick=async()=>{const reflector=$('#reflectorSelect').value,module=$('#reflectorModule').value;if(!reflector){toast('Selecione um reflector',true);return}try{await api('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({reflector,reflector_type:currentReflectorType,reflector_module:module})});configCache.reflector=reflector;configCache.reflector_type=currentReflectorType;configCache.reflector_module=module;toast(`${reflector} ${module} salvo`);await refreshStatus()}catch(e){toast(e.message,true)}};
async function loadSystem(){try{const s=await api('/api/system');applyVersion(s.version);const psram=s.psram_total?`${Math.round(s.psram_total/1048576)} MB (${s.psram_state})`:s.psram_state;const rows=[['Modelo',s.model],['Firmware',s.firmware+' v'+s.version],['ESP-IDF',s.idf],['Build',s.build_date+' '+s.build_time],['Uptime',fmtUptime(s.uptime_s)],['CPU',s.cores+' cores'],['Flash',(s.flash_bytes/1048576).toFixed(0)+' MB'],['Heap livre',Math.round(s.heap_free/1024)+' KB'],['PSRAM',psram],['Wi-Fi',s.rssi>-120?s.rssi+' dBm':'AP local'],['IP de acesso',s.ip],['IP STA',s.sta_ip],['IP AP',s.ap_ip],['MAC',s.mac]];$('#systemInfo').innerHTML=rows.map(r=>`<div class="sys-row"><span>${r[0]}</span><b>${r[1]}</b></div>`).join('');$('#buildInfo').textContent=`Build ${s.build_date} ${s.build_time} • ESP-IDF ${s.idf}`;$('#heapMini').textContent=Math.round(s.heap_free/1024)+' KB heap'}catch(e){toast(e.message,true)}}
async function loadLogs(){try{const d=await api('/api/logs');$('#logBox').textContent=d.items.map(x=>{const sec=Math.floor(x.ms/1000),h=String(Math.floor(sec/3600)).padStart(2,'0'),m=String(Math.floor((sec%3600)/60)).padStart(2,'0'),s=String(sec%60).padStart(2,'0');return `${h}:${m}:${s}  [${x.level.padEnd(6)}] ${x.text}`}).join('\n')||'Sem logs';$('#logBox').scrollTop=$('#logBox').scrollHeight}catch(e){toast(e.message,true)}}
$('#refreshLogs').onclick=loadLogs;
$('#rebootBtn').onclick=async()=>{if(!confirm('Reiniciar o Polar D-Star agora?'))return;try{await api('/api/reboot',{method:'POST'});toast('Reiniciando...')}catch(e){toast(e.message,true)}};
$('#factoryBtn').onclick=async()=>{if(!confirm('Apagar Wi-Fi e todas as configurações? Esta ação reinicia o equipamento.'))return;try{await api('/api/factory-reset',{method:'POST'});toast('Configurações apagadas. Reiniciando...')}catch(e){toast(e.message,true)}};
function fmtBytes(n){n=Number(n||0);if(!n)return '—';if(n>=1048576)return (n/1048576).toFixed(2)+' MB';if(n>=1024)return Math.round(n/1024)+' KB';return n+' B'}
function fmtReleaseDate(v){if(!v)return '—';const d=new Date(v);return Number.isNaN(d.getTime())?v:d.toLocaleString('pt-BR')}
function setUpdateText(id,value,fallback='—'){const el=$(id);if(el)el.textContent=(value===undefined||value===null||value==='')?fallback:value}
function renderOnlineUpdate(u){
  applyVersion(u.installed||statusCache.version||'0.0.0');
  setUpdateText('#installedVersion',u.installed?'v'+u.installed:'—');
  setUpdateText('#buildInfo',(u.installed_build_date&&u.installed_build_time)?(u.installed_build_date+' '+u.installed_build_time):'—');
  setUpdateText('#installedIdf',u.installed_idf);
  setUpdateText('#installedTarget',u.installed_target);
  setUpdateText('#installedPartition',u.installed_partition?(u.installed_partition+' • '+fmtBytes(u.installed_partition_size)):'—');
  setUpdateText('#installedSha',u.installed_elf_sha256);
  setUpdateText('#installedNotes',u.installed_notes,'Sem notas incorporadas nesta versão.');

  const latest=$('#onlineUpdateVersion'),state=$('#onlineUpdateState'),msg=$('#onlineUpdateMsg');
  const check=$('#checkUpdateBtn'),install=$('#onlineUpdateBtn'),bar=$('#onlineUpdateProgress'),badge=$('#onlineReleaseBadge');
  if(!latest||!state||!check||!install||!bar)return;
  latest.textContent=u.latest?('v'+u.latest):'—';
  setUpdateText('#onlinePublishedAt',fmtReleaseDate(u.latest_published_at));
  setUpdateText('#onlineIdf',u.latest_idf);
  setUpdateText('#onlineTarget',u.latest_target);
  setUpdateText('#onlineSize',fmtBytes(u.size));
  setUpdateText('#onlineSha',u.sha256);
  setUpdateText('#onlineNotes',u.latest_notes,u.checked?'Sem notas publicadas para esta release.':'Aguardando consulta ao GitHub...');

  state.textContent=u.message||'Ainda não verificado.';
  const progress=Math.max(0,Math.min(100,Number(u.progress||0)));
  bar.style.width=progress+'%';
  check.disabled=!!(u.checking||u.installing);
  install.disabled=!u.available||u.checking||u.installing;
  check.textContent=u.checking?'Verificando...':'Verificar agora';
  install.textContent=u.installing?('Atualizando '+progress+'%'):'Atualizar agora';

  if(badge){
    if(u.checking){badge.textContent='CONSULTANDO';badge.className='pill release-badge wait'}
    else if(u.available){badge.textContent='NOVA RELEASE';badge.className='pill release-badge new'}
    else if(u.checked){badge.textContent='ATUALIZADO';badge.className='pill release-badge ok'}
    else{badge.textContent='NÃO VERIFICADO';badge.className='pill release-badge wait'}
  }

  if(u.installing)msg.textContent='Baixando e gravando o firmware. Não desligue o equipamento.';
  else if(u.available)msg.textContent='Nova versão v'+u.latest+' disponível. Confira os dados acima antes de instalar.';
  else if(u.checked)msg.textContent='A release instalada é a mais recente publicada no GitHub.';
  else msg.textContent='';

  clearTimeout(updatePollTimer);
  if(u.checking||u.installing)updatePollTimer=setTimeout(()=>loadOnlineUpdate(false),1000);
}
async function loadOnlineUpdate(triggerAuto=true){
  try{
    const u=await api('/api/update');
    renderOnlineUpdate(u);
    if(triggerAuto&&!updateAutoCheckStarted&&!u.checked&&!u.checking&&!u.installing&&(u.message==='Ainda não verificado'||!u.message)){
      updateAutoCheckStarted=true;
      try{
        await api('/api/update/check',{method:'POST'});
        clearTimeout(updatePollTimer);updatePollTimer=setTimeout(()=>loadOnlineUpdate(false),500)
      }catch(e){
        const state=$('#onlineUpdateState');if(state)state.textContent=e.message;
      }
    }
  }catch(e){const state=$('#onlineUpdateState');if(state)state.textContent='Não foi possível consultar o estado da atualização.'}
}
$('#checkUpdateBtn').onclick=async()=>{
  updateAutoCheckStarted=true;
  try{
    await api('/api/update/check',{method:'POST'});
    toast('Consultando a última release no GitHub...');
    clearTimeout(updatePollTimer);updatePollTimer=setTimeout(()=>loadOnlineUpdate(false),500)
  }catch(e){toast(e.message,true)}
};
$('#onlineUpdateBtn').onclick=async()=>{
  const version=$('#onlineUpdateVersion').textContent||'nova versão';
  if(!confirm('Instalar '+version+' diretamente do GitHub? O equipamento será reiniciado.'))return;
  try{
    await api('/api/update/install',{method:'POST'});
    toast('Atualização online iniciada');
    clearTimeout(updatePollTimer);updatePollTimer=setTimeout(()=>loadOnlineUpdate(false),500)
  }catch(e){toast(e.message,true)}
};
$('#otaBtn').onclick=async()=>{const file=$('#otaFile').files[0];if(!file){toast('Selecione o arquivo .bin',true);return}if(!confirm(`Atualizar com ${file.name} (${Math.round(file.size/1024)} KB)?`))return;const btn=$('#otaBtn');btn.disabled=true;$('#otaMsg').textContent='Enviando firmware...';$('#otaProgress').style.width='15%';try{const r=await fetch('/api/ota',{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:file});$('#otaProgress').style.width='90%';const j=await r.json();if(!r.ok)throw new Error(j.error||'Falha na atualização');$('#otaProgress').style.width='100%';$('#otaMsg').textContent='Firmware gravado. Reiniciando...';toast('OTA concluída')}catch(e){$('#otaMsg').textContent=e.message;toast(e.message,true);btn.disabled=false;$('#otaProgress').style.width='0'}};
function drawChart(){const c=$('#signalChart');if(!c)return;const dpr=devicePixelRatio||1,rect=c.getBoundingClientRect();c.width=Math.max(320,rect.width*dpr);c.height=Math.max(180,rect.height*dpr);const x=c.getContext('2d');x.scale(dpr,dpr);const w=rect.width,h=rect.height;x.clearRect(0,0,w,h);x.strokeStyle='#17394b';x.lineWidth=1;for(let i=1;i<6;i++){x.beginPath();x.moveTo(0,h*i/6);x.lineTo(w,h*i/6);x.stroke()}for(let i=1;i<12;i++){x.beginPath();x.moveTo(w*i/12,0);x.lineTo(w*i/12,h);x.stroke()}x.strokeStyle='#1de965';x.lineWidth=2;x.beginPath();for(let i=0;i<=120;i++){const px=w*i/120;const py=h*.72+Math.sin(i*.55)*3+Math.sin(i*.11)*5;if(i===0)x.moveTo(px,py);else x.lineTo(px,py)}x.stroke();x.fillStyle='#7596a8';x.font='12px system-ui';x.fillText('SIMULAÇÃO VISUAL — dados RF entram na v0.2',12,20)}
window.addEventListener('resize',()=>{if($('#page-radio').classList.contains('active'))drawChart()});
(async()=>{const initial=(location.hash||'#dashboard').slice(1);buildModuleOptions('D');showPage(initial);await Promise.all([refreshStatus(),loadConfig(),loadSystem(),loadWifi(),loadHostsStatus()]);setInterval(refreshStatus,3000)})();
