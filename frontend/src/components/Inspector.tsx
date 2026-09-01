import { useState } from 'react'
import { formatByte } from '../graph/model'
import { adjacentJoin, formatOffset, joinIndex, pathEdges } from '../forensics/model'
import type { Trace } from '../trace/types'
import type { Selection } from './FragmentGraph'

type InspectSelection = Extract<NonNullable<Selection>, {kind:'fragment'|'edge'}> | undefined
const Line=({a,b}:{a:string;b:React.ReactNode})=><div className="inspect-line"><span>{a}</span><b>{b}</b></div>
const Repair=({trace}:{trace:Trace})=><div className="repair"><h2>Repair summary</h2><Line a="Repairs performed" b={trace.repairs.length}/><Line a="Consensus" b={trace.repairs.filter(r=>r.stage==='consensus').length}/><Line a="PNG CRC-guided" b={trace.repairs.filter(r=>r.stage==='png_crc').length}/></div>

export function Inspector({trace,selection,onSelect}:{trace:Trace;selection:InspectSelection;onSelect?:(s:Selection)=>void}) {
  const [mismatchOnly,setMismatchOnly]=useState(false)
  if(!selection)return <aside className="inspector"><h2>Evidence inspector</h2><p>Select a fragment or relationship to inspect observed evidence.</p><Repair trace={trace}/></aside>
  if(selection.kind==='fragment'){
    const f=selection.value,p=trace.selected_path.indexOf(f.id)
    return <aside className="inspector"><h2>Fragment evidence</h2><Line a="ID" b={`F${f.id}`}/><Line a="Name" b={f.name}/><Line a="Size" b={`${f.size} B`}/><Line a="Selected path" b={p>=0?'Yes':'No'}/><Line a="Path position" b={p>=0?p+1:'—'}/><Line a="Incoming" b={trace.edges.filter(e=>e.to===f.id).length}/><Line a="Outgoing" b={trace.edges.filter(e=>e.from===f.id).length}/><Repair trace={trace}/></aside>
  }
  const e=selection.value,index=joinIndex(trace,e),joins=pathEdges(trace)
  const navigate=(delta:number)=>{const next=adjacentJoin(trace,e,delta,mismatchOnly);if(next)onSelect?.({kind:'edge',value:next})}
  return <aside className="inspector forensic"><div className="eyebrow">EDGE F{e.from} → F{e.to}</div><h2>Overlap evidence</h2><Line a="Type" b={e.exact?'Exact overlap':'Approximate overlap'}/><Line a="Overlap" b={`${e.overlap} bytes`}/><Line a="Matches" b={e.matches}/><Line a="Mismatches" b={e.mismatches}/><Line a="Selected path" b={index>=0?'Yes':'No'}/>{index>=0&&<Line a="Path context" b={`Join ${index+1} of ${joins.length}`}/>}<div className="nav"><button disabled={!adjacentJoin(trace,e,-1,mismatchOnly)} onClick={()=>navigate(-1)}>Previous {mismatchOnly?'Mismatch':'Join'}</button><button disabled={!adjacentJoin(trace,e,1,mismatchOnly)} onClick={()=>navigate(1)}>Next {mismatchOnly?'Mismatch':'Join'}</button></div><div className="inspect-filters"><label><input type="checkbox" checked readOnly/> Selected-path joins only</label><label><input type="checkbox" checked={mismatchOnly} onChange={x=>setMismatchOnly(x.target.checked)}/> Mismatches only</label></div>{e.mismatch_details.length?<><h2>Observed disagreements</h2><table><thead><tr><th>Offset</th><th>Left</th><th>Right</th><th>Status</th></tr></thead><tbody>{e.mismatch_details.map((m,i)=><tr key={i}><td>{formatOffset(m.overlap_offset)}</td><td>{formatByte(m.left_byte)}</td><td>{formatByte(m.right_byte)}</td><td>disagreement</td></tr>)}</tbody></table><div className="unresolved">Unresolved disagreement</div></>:<div className="clean-evidence">No byte disagreements were observed in this overlap.</div>}<Repair trace={trace}/></aside>
}
