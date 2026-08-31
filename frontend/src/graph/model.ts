import type{EdgeTrace,Trace}from'../trace/types'
export const pair=(a:number,b:number)=>`${a}->${b}`
export const selectedPairs=(path:number[])=>new Set(path.slice(1).map((id,i)=>pair(path[i],id)))
export function filterEdges(trace:Trace,exact=true,approx=true,path=true){const chosen=selectedPairs(trace.selected_path);return trace.edges.filter(e=>(e.exact?exact:approx)&&(path||!chosen.has(pair(e.from,e.to))))}
export const formatByte=(v:number)=>`0x${v.toString(16).toUpperCase().padStart(2,'0')}`
export function positions(trace:Trace){const chosen=new Map(trace.selected_path.map((id,i)=>[id,i]));let other=0;return new Map(trace.fragments.map(f=>{const p=chosen.get(f.id);return[f.id,p===undefined?{x:120+(other++%3)*220,y:210+Math.floor((other-1)/3)*130}:{x:80+p*230,y:70}]}))}
export const edgeLabel=(e:EdgeTrace)=>e.exact?`${e.overlap} B`:`${e.overlap} B · ${e.mismatches} mismatch${e.mismatches===1?'':'es'}`
