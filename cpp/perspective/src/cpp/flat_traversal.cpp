/******************************************************************************
 *
 * Copyright (c) 2017, the Perspective Authors.
 *
 * This file is part of the Perspective library, distributed under the terms of
 * the Apache License 2.0.  The full license can be found in the LICENSE file.
 *
 */

#include <perspective/first.h>
#include <perspective/base.h>
#include <perspective/config.h>
#include <perspective/flat_traversal.h>
#include <perspective/scalar.h>
#include <perspective/schema.h>
#ifdef PSP_PARALLEL_FOR
#include <tbb/parallel_sort.h>
#endif

namespace perspective {

t_ftrav::t_ftrav(bool handle_nan_sort)
    : m_step_deletes(0)
    , m_step_inserts(0)
    , m_handle_nan_sort(handle_nan_sort) {
    m_index = std::make_shared<std::vector<t_mselem>>();
}

void
t_ftrav::init() {
    m_index = std::make_shared<std::vector<t_mselem>>();
}

std::vector<t_tscalar>
t_ftrav::get_all_pkeys(const std::vector<std::pair<t_uindex, t_uindex>>& cells) const {
    // assumes the code calling this has already validated
    // cells
    std::vector<t_tscalar> rval;
    rval.reserve(cells.size());
    std::vector<t_mselem>* index = m_index.get();
    for (auto iter = cells.begin(); iter != cells.end(); ++iter) {
        rval.push_back((*index)[iter->first].m_pkey);
    }
    return rval;
}

std::vector<t_tscalar>
t_ftrav::get_pkeys(const std::vector<std::pair<t_uindex, t_uindex>>& cells) const {
    std::unordered_set<t_tscalar> all_pkeys;

    std::set<t_index> all_rows;

    for (t_index idx = 0, loop_end = cells.size(); idx < loop_end; ++idx) {
        all_rows.insert(cells[idx].first);
    }

    std::vector<t_tscalar> rval(all_rows.size());
    std::set<t_index>::iterator it;
    t_index count = 0;
    for (it = all_rows.begin(); it != all_rows.end(); ++it) {
        rval[count] = (*m_index)[*it].m_pkey;
        ++count;
    }
    return rval;
}

std::vector<t_tscalar>
t_ftrav::get_pkeys(t_index begin_row, t_index end_row) const {
    t_index index_size = m_index->size();
    end_row = std::min(end_row, index_size);
    std::vector<t_tscalar> rval(end_row - begin_row);
    for (t_index ridx = begin_row; ridx < end_row; ++ridx) {
        rval[ridx - begin_row] = (*m_index)[ridx].m_pkey;
    }
    return rval;
}

std::vector<t_tscalar>
t_ftrav::get_pkeys() const {
    return get_pkeys(0, size());
}

t_tscalar
t_ftrav::get_pkey(t_index idx) const {
    return (*m_index)[idx].m_pkey;
}

void
t_ftrav::fill_sort_elem(std::shared_ptr<const t_gstate> state, const t_config& config,
    t_tscalar pkey, t_mselem& out_elem) {
    out_elem.m_pkey = pkey;
    t_index sortby_size = m_sortby.size();
    out_elem.m_row.reserve(sortby_size);
    for (t_index idx = 0; idx < sortby_size; ++idx) {
        t_index sortby_idx = m_sortby[idx].m_agg_index;
        const std::string& colname = config.col_at(sortby_idx);
        const std::string sortby_colname = config.get_sort_by(colname);
        out_elem.m_row.push_back(
            m_symtable.get_interned_tscalar(state->get(pkey, sortby_colname)));
    }
}

void
t_ftrav::fill_sort_elem(std::shared_ptr<const t_gstate> state, const t_config& config,
    const std::vector<t_tscalar>& row, t_mselem& out_elem) const {
    out_elem.m_pkey = mknone();
    t_index sortby_size = m_sortby.size();
    out_elem.m_row.reserve(sortby_size);
    for (t_index idx = 0; idx < sortby_size; ++idx) {
        t_index sortby_idx = m_sortby[idx].m_agg_index;
        const std::string& colname = config.col_at(sortby_idx);
        const std::string sortby_colname = config.get_sort_by(colname);
        out_elem.m_row.push_back(
            get_interned_tscalar(row.at(config.get_colidx(sortby_colname))));
    }
}

void
t_ftrav::sort_by(std::shared_ptr<const t_gstate> state, const t_config& config,
    const std::vector<t_sortspec>& sortby) {
    if (sortby.empty())
        return;
    t_multisorter sorter(get_sort_orders(sortby), m_handle_nan_sort);
    t_index size = m_index->size();
    auto sort_elems = std::make_shared<std::vector<t_mselem>>(static_cast<size_t>(size));
    m_sortby = sortby;

    for (t_index idx = 0; idx < size; ++idx) {
        t_mselem& elem = (*sort_elems)[idx];
        t_tscalar pkey = (*m_index)[idx].m_pkey;
        fill_sort_elem(state, config, pkey, elem);
    }

    std::swap(m_index, sort_elems);
    std::sort(m_index->begin(), m_index->end(), sorter);
    m_pkeyidx.clear();
    for (t_index idx = 0, loop_end = m_index->size(); idx < loop_end; ++idx) {
        m_pkeyidx[(*m_index)[idx].m_pkey] = idx;
    }
}

t_index
t_ftrav::size() const {
    return m_index->size();
}

void
t_ftrav::get_row_indices(const std::unordered_set<t_tscalar>& pkeys,
    std::unordered_map<t_tscalar, t_index>& out_map) const {
    for (t_index idx = 0, loop_end = size(); idx < loop_end; ++idx) {
        const t_tscalar& pkey = (*m_index)[idx].m_pkey;
        if (pkeys.find(pkey) != pkeys.end()) {
            out_map[pkey] = idx;
        }
    }
}

void
t_ftrav::get_row_indices(t_index bidx, t_index eidx, const std::unordered_set<t_tscalar>& pkeys,
    std::unordered_map<t_tscalar, t_index>& out_map) const {
    for (t_index idx = bidx; idx < eidx; ++idx) {
        const t_tscalar& pkey = (*m_index)[idx].m_pkey;
        if (pkeys.find(pkey) != pkeys.end()) {
            out_map[pkey] = idx;
        }
    }
}

void
t_ftrav::reset() {
    if (m_index.get())
        m_index->clear();
}

void
t_ftrav::check_size() {
    std::unordered_set<t_tscalar> pkey_set;
    for (t_index idx = 0, loop_end = m_index->size(); idx < loop_end; ++idx) {
        if (pkey_set.find((*m_index)[idx].m_pkey) != pkey_set.end()) {
            std::cout << "Duplicate entry for " << (*m_index)[idx].m_pkey << std::endl;
            PSP_COMPLAIN_AND_ABORT("Exiting");
        }

        pkey_set.insert((*m_index)[idx].m_pkey);
    }
}

bool
t_ftrav::validate_cells(const std::vector<std::pair<t_uindex, t_uindex>>& cells) const {
    t_index trav_size = size();

    for (t_index idx = 0, loop_end = cells.size(); idx < loop_end; ++idx) {
        t_index ridx = cells[idx].first;
        if (ridx >= trav_size)
            return false;
    }
    return true;
}

void
t_ftrav::step_begin() {
    m_step_deletes = 0;
    m_step_inserts = 0;
    m_new_elems.clear();
}

void
t_ftrav::step_end() {
    t_index new_size = m_index->size() + m_step_inserts - m_step_deletes;

    auto new_index = std::make_shared<std::vector<t_mselem>>();
    new_index->reserve(new_size);

    t_pkeyidx_map added;

    for (t_index idx = 0, loop_end = m_index->size(); idx < loop_end; ++idx) {
        t_mselem& elem = (*m_index)[idx];
        if (!elem.m_deleted) {
            new_index->push_back(elem);
            added[elem.m_pkey] = idx;
        }
    }

    for (t_pkmselem_map::const_iterator pkelem_iter = m_new_elems.begin();
         pkelem_iter != m_new_elems.end(); ++pkelem_iter) {

        const t_mselem& elem = pkelem_iter->second;
        t_pkeyidx_map::const_iterator iter = added.find(pkelem_iter->first);
        if (iter == added.end()) {
            new_index->push_back(elem);
        } else {
            (*new_index)[iter->second] = elem;
        }
    }
    std::swap(new_index, m_index);
    t_multisorter sorter(get_sort_orders(m_sortby), m_handle_nan_sort);
    std::sort(m_index->begin(), m_index->end(), sorter);

    m_pkeyidx.clear();

    m_new_elems.clear();

    for (t_index idx = 0, loop_end = m_index->size(); idx < loop_end; ++idx) {
        m_pkeyidx[(*m_index)[idx].m_pkey] = idx;
    }
}

void
t_ftrav::add_row(
    std::shared_ptr<const t_gstate> state, const t_config& config, t_tscalar pkey) {
    t_mselem mselem;
    fill_sort_elem(state, config, pkey, mselem);
    m_new_elems[pkey] = mselem;
    ++m_step_inserts;
}

void
t_ftrav::update_row(
    std::shared_ptr<const t_gstate> state, const t_config& config, t_tscalar pkey) {
    if (m_sortby.empty())
        return;
    t_pkeyidx_map::iterator pkiter = m_pkeyidx.find(pkey);
    if (pkiter == m_pkeyidx.end()) {
        add_row(state, config, pkey);
        return;
    }
    t_mselem mselem;
    fill_sort_elem(state, config, pkey, mselem);
    (*m_index)[pkiter->second] = mselem;
}

void
t_ftrav::delete_row(t_tscalar pkey) {
    t_pkeyidx_map::iterator pkiter = m_pkeyidx.find(pkey);
    if (pkiter == m_pkeyidx.end())
        return;
    (*m_index)[pkiter->second].m_deleted = true;
    m_new_elems.erase(pkey);
    ++m_step_deletes;
}

std::vector<t_sortspec>
t_ftrav::get_sort_by() const {
    return m_sortby;
}

bool
t_ftrav::empty_sort_by() const {
    return m_sortby.empty();
}

void
t_ftrav::reset_step_state() {
    m_step_deletes = 0;
    m_step_inserts = 0;
    m_new_elems.clear();
}

t_uindex
t_ftrav::lower_bound_row_idx(std::shared_ptr<const t_gstate> state, const t_config& config,
    const std::vector<t_tscalar>& row) const {
    t_multisorter sorter(get_sort_orders(m_sortby), m_handle_nan_sort);
    t_mselem target_val;

    fill_sort_elem(state, config, row, target_val);

    auto iter = std::lower_bound(m_index->begin(), m_index->end(), target_val, sorter);

    return std::distance(m_index->begin(), iter);
}

t_index
t_ftrav::get_row_idx(t_tscalar pkey) const {
    auto pkiter = m_pkeyidx.find(pkey);
    if (pkiter == m_pkeyidx.end())
        return -1;
    return pkiter->second;
}

} // end namespace perspective
