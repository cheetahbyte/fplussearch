use fsearch::{content::{Content, Grep}, index::Index, live::Live, query::{GrepMode, Query, Searcher}, walk};
use serde_json::{json, Value};
use std::{io::{BufRead, BufReader, Write}, os::unix::net::UnixListener, path::PathBuf, time::Instant};

fn main() {
    fsearch::no_materialize();
    let args: Vec<String> = std::env::args().collect();
    let root = &args[1];
    let dir = PathBuf::from(&args[2]);
    std::fs::create_dir_all(&dir).unwrap();
    let started = Instant::now();
    let components: Vec<&str> = root.split('/').filter(|s| !s.is_empty()).collect();
    let offset = components.len() as u32;
    let mut listings = walk::scan(root.as_bytes(), 8);
    for listing in &mut listings {
        listing.id += offset;
        for entry in &mut listing.ents {
            if entry.child != walk::NONE { entry.child += offset; }
        }
    }
    let mut parent = String::from("/");
    for (id, component) in components.iter().enumerate() {
        let mut listing = walk::list_one(parent.as_bytes()).unwrap();
        listing.id = id as u32;
        listing.ents.retain(|e| &listing.names[e.name_off as usize..][..e.name_len as usize] == component.as_bytes());
        assert_eq!(listing.ents.len(), 1);
        listing.ents[0].child = id as u32 + 1;
        listings.push(listing);
        if parent != "/" { parent.push('/'); }
        parent.push_str(component);
    }
    let idx = Index::build(listings, 0, fsearch::query::now_secs(), root.as_bytes());
    idx.save(&dir.join("index.bin")).unwrap();
    drop(idx);
    let live = Live::new(Index::load(&dir.join("index.bin")).unwrap());
    let name_ready = started.elapsed().as_secs_f64();
    let mut content = Content::open(dir.join("content"));
    let docs = fsearch::content::wanted(&live, root.as_bytes(), root.as_bytes(), true);
    for batch in docs.batches() {
        let id = content.alloc_id();
        content.push(fsearch::content::build_segment(&dir.join("content"), id, &docs, batch).unwrap());
    }
    drop(docs);
    while let Some(ids) = content.merge_plan() {
        let id = content.alloc_id();
        let segment = fsearch::content::merge(&dir.join("content"), id, &content.segments(&ids)).unwrap();
        content.replace(&ids, segment);
    }
    let pool = rayon::ThreadPoolBuilder::new().start_handler(|_| unsafe {
        libc::pthread_set_qos_class_self_np(libc::qos_class_t::QOS_CLASS_USER_INTERACTIVE, 0);
    }).build().unwrap();
    unsafe extern "C" {
        fn malloc_zone_pressure_relief(zone: *mut std::ffi::c_void, goal: usize) -> usize;
    }
    unsafe { malloc_zone_pressure_relief(std::ptr::null_mut(), 0); }
    let total_build = started.elapsed().as_secs_f64();
    let listener = UnixListener::bind(dir.join("search.sock")).unwrap();
    eprintln!("ready: {} entries, {} content docs", live.base.n, content.docs());
    let (mut stream, _) = listener.accept().unwrap();
    let reader = BufReader::new(stream.try_clone().unwrap());
    for line in reader.lines() {
        let req: Value = serde_json::from_str(&line.unwrap()).unwrap();
        let response = if req["op"] == "status" {
            json!({"ok":true,"entries":live.base.n,"content_docs":content.docs(),"content_pending":false,"name_build_s":name_ready,"total_build_s":total_build})
        } else {
            let mut q = Query::parse(req["q"].as_str().unwrap_or(""), root).unwrap();
            q.limit = req["limit"].as_u64().unwrap_or(50) as usize;
            if req["op"] == "grep" {
                let mut g = Grep::new(req["pattern"].as_str().unwrap(), GrepMode::Literal).unwrap();
                g.max_per_file = 1;
                g.budget = None;
                let r = content.search(&g, &q);
                json!({"ok":true,"complete":r.complete,"files":r.files.iter().map(|f| json!({"path":String::from_utf8_lossy(&f.path),"lines":f.lines.iter().map(|(line,text)| json!({"line":line,"text":text})).collect::<Vec<_>>()})).collect::<Vec<_>>()})
            } else {
                let hits = pool.install(|| Searcher { live: &live }.search(&q));
                let paths: Vec<Value> = hits.iter().map(|h| {
                    let mut path = Vec::new();
                    live.base.path(h.idx as usize, &mut path);
                    json!({"path":String::from_utf8_lossy(&path)})
                }).collect();
                json!({"ok":true,"hits":paths})
            }
        };
        writeln!(stream, "{response}").unwrap();
        stream.flush().unwrap();
    }
}
